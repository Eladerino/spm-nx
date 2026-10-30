#pragma once

// The guest profiler's report maths, shared by its capture backends and kept free of platform
// and registry dependencies so it can be tested on a host: given the translated functions'
// host entry points and a set of sampled host addresses, decide which function each sample
// belongs to and format the hot list (guest_profiler.cpp logs the lines). A sample belongs to
// the function whose entry point most recently precedes it, the same rule
// TranslatedFunctionRegistry::FindByHostAddress applies live; the Switch backend resolves
// through that registry and hands the results here, the tests hand over fabricated entries.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GuestProfileFormat {

struct Entry {
    uintptr_t host = 0;    // the translated function's host entry point
    uint32_t guest = 0;    // its guest address
    const char* name = ""; // its name (registry or decomp map); may be empty, never null
};

// The entry a host address falls in, or null if it precedes the first entry. Entries are
// sorted by host. Past the last entry only 256 KiB is claimed, matching FindByHostAddress:
// beyond that the address is some other code, not a long tail of the last function.
inline const Entry* FindEntry(const std::vector<Entry>& entries, uintptr_t pc) {
    if (entries.empty() || pc < entries.front().host) {
        return nullptr;
    }
    const auto it = std::upper_bound(entries.begin(), entries.end(), pc,
                                     [](uintptr_t value, const Entry& e) { return value < e.host; });
    const Entry& entry = *(it - 1);
    if (it == entries.end() && pc - entry.host >= 256u * 1024u) {
        return nullptr;
    }
    return &entry;
}

// A sampled address as report text: the guest function it falls in ("800AB123 Name"), or,
// for code no translated function claims, its offset from the module base ("nro+1A2B3",
// resolvable against the build's symbol table), or the raw address when even that does not
// apply (below the module base: another mapping entirely).
inline std::string DescribeAddress(const std::vector<Entry>& entries, uintptr_t pc, uintptr_t moduleBase) {
    if (const Entry* entry = FindEntry(entries, pc)) {
        char text[256];
        if (entry->name != nullptr && entry->name[0] != '\0') {
            std::snprintf(text, sizeof(text), "%08X %s", entry->guest, entry->name);
        } else {
            std::snprintf(text, sizeof(text), "%08X", entry->guest);
        }
        return text;
    }
    char text[48];
    if (moduleBase != 0 && pc >= moduleBase) {
        std::snprintf(text, sizeof(text), "nro+%llX", static_cast<unsigned long long>(pc - moduleBase));
    } else {
        std::snprintf(text, sizeof(text), "host 0x%llX", static_cast<unsigned long long>(pc));
    }
    return text;
}

// Leaf samples aggregated by the function each falls in, hottest first. Count ties break on
// the description so the output order is stable between runs.
inline std::vector<std::pair<std::string, uint32_t>> AggregateLeaves(
    const std::unordered_map<uintptr_t, uint32_t>& samples, const std::vector<Entry>& entries,
    uintptr_t moduleBase) {
    std::unordered_map<std::string, uint32_t> byName;
    for (const auto& [pc, count] : samples) {
        byName[DescribeAddress(entries, pc, moduleBase)] += count;
    }
    std::vector<std::pair<std::string, uint32_t>> sorted(byName.begin(), byName.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    return sorted;
}

// The hot list itself, one "[profile] <share>  <function>" line per entry, hottest first.
inline std::vector<std::string> LeafLines(const std::unordered_map<uintptr_t, uint32_t>& samples,
                                          const std::vector<Entry>& entries, uintptr_t moduleBase,
                                          uint32_t total, size_t limit) {
    std::vector<std::string> lines;
    if (total == 0) {
        return lines;
    }
    const auto sorted = AggregateLeaves(samples, entries, moduleBase);
    for (size_t i = 0; i < sorted.size() && i < limit; ++i) {
        char text[320];
        std::snprintf(text, sizeof(text), "[profile] %5.1f%%  %s", 100.0 * sorted[i].second / total,
                      sorted[i].first.c_str());
        lines.emplace_back(text);
    }
    return lines;
}

// The top call chains: each captured stack (leaf first) described frame by frame and counted,
// hottest first. Unlike the Windows backend's chains, which isolated where stacks enter
// system code, these run through the guest frames on purpose: the caller behind a hot leaf
// is what hot ordering and hot-leaf inlining act on. Shares are of all samples taken.
inline std::vector<std::string> ChainLines(const std::vector<std::vector<uintptr_t>>& stacks,
                                           const std::vector<Entry>& entries, uintptr_t moduleBase,
                                           uint32_t total, size_t limit, size_t maxFrames = 5) {
    std::vector<std::string> lines;
    if (total == 0) {
        return lines;
    }
    std::unordered_map<std::string, uint32_t> chains;
    for (const auto& stack : stacks) {
        std::string chain;
        const size_t depth = stack.size() < maxFrames ? stack.size() : maxFrames;
        for (size_t i = 0; i < depth; ++i) {
            chain += (chain.empty() ? "" : " < ") + DescribeAddress(entries, stack[i], moduleBase);
        }
        if (!chain.empty()) {
            chains[chain]++;
        }
    }
    std::vector<std::pair<std::string, uint32_t>> sorted(chains.begin(), chains.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    for (size_t i = 0; i < sorted.size() && i < limit; ++i) {
        char text[1024];
        std::snprintf(text, sizeof(text), "[profile] chain: %5.1f%%  %s", 100.0 * sorted[i].second / total,
                      sorted[i].first.c_str());
        lines.emplace_back(text);
    }
    return lines;
}

} // namespace GuestProfileFormat
