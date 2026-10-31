// Calls per native override per perftimer window: the [calls] lines.
//
// Every PPC_NATIVE_OVERRIDE carries a NativeOverrideCounter (hle_stubs.h), linked here at static
// init. At the end of each perftimer window aurora calls PrintNativeOverrideCounts, which prints
// the overrides that ran in it, busiest first, as calls per frame. The GX worker design uses these
// to tell which GX calls run often enough that the game thread must not wait on them.

#include "hle_stubs.h"

#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

// Constant-initialized, so counters constructed at static init in any order can link in.
NativeOverrideCounter* g_nativeOverrideCounterHead = nullptr;

// The [calls] lines list the overrides averaging at least kOverrideCountMinPerFrame calls a frame,
// at most kOverrideCountMaxEntries of them, kOverrideCountPerLine to a line.
constexpr double kOverrideCountMinPerFrame = 0.05;
constexpr size_t kOverrideCountMaxEntries = 60;
constexpr size_t kOverrideCountPerLine = 6;

void PrintNativeOverrideCounts(uint64_t firstFrame, uint64_t lastFrame) {
    if (lastFrame < firstFrame) {
        return;
    }
    const double frames = static_cast<double>(lastFrame - firstFrame + 1);
    std::vector<std::pair<uint64_t, const char*>> ran;
    uint64_t total = 0;
    for (NativeOverrideCounter* c = g_nativeOverrideCounterHead; c != nullptr; c = c->next) {
        const uint64_t calls = c->calls.load(std::memory_order_relaxed);
        const uint64_t delta = calls - c->printed;
        c->printed = calls;
        if (delta == 0) {
            continue;
        }
        total += delta;
        if (static_cast<double>(delta) / frames >= kOverrideCountMinPerFrame) {
            ran.emplace_back(delta, c->name);
        }
    }
    if (ran.empty()) {
        return;
    }
    std::sort(ran.begin(), ran.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (ran.size() > kOverrideCountMaxEntries) {
        ran.resize(kOverrideCountMaxEntries);
    }
    std::fprintf(stderr, "[calls] frames %llu-%llu: %.0f override calls a frame; per frame, busiest first:\n",
                 static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame),
                 static_cast<double>(total) / frames);
    std::string line;
    for (size_t i = 0; i < ran.size(); ++i) {
        char item[160];
        std::snprintf(item, sizeof(item), "%s%s %.1f", line.empty() ? "" : ", ", ran[i].second,
                      static_cast<double>(ran[i].first) / frames);
        line += item;
        if ((i + 1) % kOverrideCountPerLine == 0 || i + 1 == ran.size()) {
            std::fprintf(stderr, "[calls]   %s\n", line.c_str());
            line.clear();
        }
    }
}

// Set at static init: aurora keeps the hook in a constant-initialized atomic, so the order of
// static initialization between the two does not matter.
[[maybe_unused]] const bool g_nativeOverrideHookSet = (AuroraSetPerfWindowHook(&PrintNativeOverrideCounts), true);

} // namespace

NativeOverrideCounter::NativeOverrideCounter(const char* counterName) noexcept : name(counterName) {
    next = g_nativeOverrideCounterHead;
    g_nativeOverrideCounterHead = this;
}
