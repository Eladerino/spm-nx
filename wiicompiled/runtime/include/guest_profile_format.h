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
    // A native override (an HLE function): its code is one C++ function of unknown length, and what
    // follows it in the module up to the next registered entry is other host code of the same object
    // file or the next one. The entry still claims that range, which keeps the leaf list grouped, but
    // the report names the exact addresses too so the build's symbol table can say what really ran.
    bool native = false;
};

// An address as an offset from the module base ("nro+1A2B3", resolvable against the build's symbol
// table, switch/scripts/symbolize.sh), or the raw address when it is below the base (another
// mapping entirely).
inline std::string HostOffset(uintptr_t pc, uintptr_t moduleBase) {
    char text[48];
    if (moduleBase != 0 && pc >= moduleBase) {
        std::snprintf(text, sizeof(text), "nro+%llX", static_cast<unsigned long long>(pc - moduleBase));
    } else {
        std::snprintf(text, sizeof(text), "host 0x%llX", static_cast<unsigned long long>(pc));
    }
    return text;
}

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

// A sampled address as report text: the guest function it falls in ("800AB123 Name"), or, for
// code no translated function claims, its module offset (HostOffset). With exactNative, an address
// in a native entry's range is given as its module offset as well: a call chain needs the exact
// caller, not the HLE function that happens to precede it in the module.
inline std::string DescribeAddress(const std::vector<Entry>& entries, uintptr_t pc, uintptr_t moduleBase,
                                   bool exactNative = false) {
    const Entry* entry = FindEntry(entries, pc);
    if (entry != nullptr && !(exactNative && entry->native)) {
        char text[256];
        if (entry->name != nullptr && entry->name[0] != '\0') {
            std::snprintf(text, sizeof(text), "%08X %s", entry->guest, entry->name);
        } else {
            std::snprintf(text, sizeof(text), "%08X", entry->guest);
        }
        return text;
    }
    return HostOffset(pc, moduleBase);
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
    // The exact addresses behind each native entry's share (see Entry::native), hottest first.
    std::unordered_map<std::string, std::unordered_map<uintptr_t, uint32_t>> nativePcs;
    for (const auto& [pc, count] : samples) {
        if (const Entry* entry = FindEntry(entries, pc); entry != nullptr && entry->native) {
            nativePcs[DescribeAddress(entries, pc, moduleBase)][pc] += count;
        }
    }
    for (size_t i = 0; i < sorted.size() && i < limit; ++i) {
        char text[320];
        std::snprintf(text, sizeof(text), "[profile] %5.1f%%  %s", 100.0 * sorted[i].second / total,
                      sorted[i].first.c_str());
        std::string line = text;
        if (const auto it = nativePcs.find(sorted[i].first); it != nativePcs.end()) {
            std::vector<std::pair<uintptr_t, uint32_t>> pcs(it->second.begin(), it->second.end());
            std::sort(pcs.begin(), pcs.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
            });
            line += "  (native; hottest";
            for (size_t k = 0; k < pcs.size() && k < 3; ++k) {
                line += ' ';
                line += HostOffset(pcs[k].first, moduleBase);
            }
            line += ')';
        }
        lines.emplace_back(std::move(line));
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
            chain += (chain.empty() ? "" : " < ") + DescribeAddress(entries, stack[i], moduleBase, /*exactNative=*/true);
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

// Inclusive shares of the native (HLE) entries: for each one, the samples whose stack passes
// through it anywhere, counted once per stack. The leaf list splits host work into the many small
// helpers it ends in; this says how much of the game thread sits under each host entry point the
// guest calls, which is the work that could move to another core. The first line is the share of
// samples under any native entry at all. Frames past the capture depth are not seen, so these are
// lower bounds.
inline std::vector<std::string> InclusiveNativeLines(const std::vector<std::vector<uintptr_t>>& stacks,
                                                     const std::vector<Entry>& entries, uintptr_t moduleBase,
                                                     uint32_t total, size_t limit) {
    std::vector<std::string> lines;
    if (total == 0) {
        return lines;
    }
    std::unordered_map<std::string, uint32_t> byName;
    uint32_t underAny = 0;
    std::vector<std::string> seen;
    for (const auto& stack : stacks) {
        seen.clear();
        for (const uintptr_t pc : stack) {
            const Entry* entry = FindEntry(entries, pc);
            if (entry == nullptr || !entry->native) {
                continue;
            }
            std::string name = DescribeAddress(entries, pc, moduleBase);
            if (std::find(seen.begin(), seen.end(), name) == seen.end()) {
                seen.emplace_back(std::move(name));
            }
        }
        if (!seen.empty()) {
            ++underAny;
        }
        for (const auto& name : seen) {
            byName[name]++;
        }
    }
    char text[320];
    std::snprintf(text, sizeof(text), "[profile] inclusive: %5.1f%%  under any native entry", 100.0 * underAny / total);
    lines.emplace_back(text);
    std::vector<std::pair<std::string, uint32_t>> sorted(byName.begin(), byName.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    for (size_t i = 0; i < sorted.size() && i < limit; ++i) {
        std::snprintf(text, sizeof(text), "[profile] inclusive: %5.1f%%  %s", 100.0 * sorted[i].second / total,
                      sorted[i].first.c_str());
        lines.emplace_back(text);
    }
    return lines;
}

} // namespace GuestProfileFormat
