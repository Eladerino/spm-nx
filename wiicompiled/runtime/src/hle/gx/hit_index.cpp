// The hitNameToPtr persistent index (hit_index.h; spm-hit-index design (b)).
//
// The helper runs inside the one translated body of hitNameToPtr (guest
// 0x8006F808), patched in on the build work copy by
// switch/scripts/patch-hitindex.py: an entry block right after the body's
// argument capture (`r25 = r3;`, before the mapGetWork inline) and a
// compare block at the head of the common return block loc_8006F8A0.
// Eleven writer hooks in the hit-family bodies bump the single global
// generation: hitEntry exit and hitReinit entry (structure), hitEntryMOBJ
// exit and _hitEnt exit (fill), hitDelete at loc_8006BA24 (delete), and
// the six flag bodies' exits (flags) - hitFlgOn / hitFlgOff plus the four
// wrappers hitObjFlagOn / hitObjFlagOff / hitGrpFlagOn / hitGrpFlagOff,
// which write entry+0 directly with their own caller masks and so are
// NOT covered by the workers' hooks (census amendment, H8-H11).
//
// The rebuild walk reads exactly the inputs the scan reads, at the scan's
// offsets, pinned instruction-by-instruction against the generated body
// (spm-s77-design-evidence.md as corrected by the s77 insanity diagnosis):
// the work base from the SDA map-work slot (r13-32528), the slot index at
// workBase+1688, the group count at slot+0 - groups AT slot+0, stride 420
// (a slot is 2x420 plus 4 trailing bytes; there is no +4 header) - and per
// group the entry count at +392 and the entries base at +396; entries of
// 276 B, skipped when the u16 at +0 has bit 0x80 set; the compared name is
// *(char**)(*(u32*)(entry+8)), reached by two dereferences; first match in
// (group, entry) order wins; a miss is 0; a null name pointer is 0 without
// a scan.
//
// Guest memory is read with the same MemoryInline flat primitives the
// generated body uses (memory_access.h). On the flat guest view any u32
// address is readable and unmapped touches read as zero, so the sanity
// bounds below are about lengths and counts only; "unreadable" does not
// exist as a case.
//
// Threading: guest bodies execute only on the scheduler/game thread, so no
// lock is taken. The invariant is not trusted blindly: the first active
// call's thread id is recorded (through std::call_once, bbox review F2)
// and any later call on a different thread declines (counted offthread)
// before touching the index state. The generation is a relaxed atomic, so
// a bump is a defined operation from any thread; everything else in this
// file is touched only on the owner thread.

#include "hit_index.h"

#include "memory_access.h"
#include "ppc_runtime.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace HitIndex {
namespace {

// ---- bounds and layout -------------------------------------------------------
// Scope sanity: slot indices at or below kMaxSlotIdx (the game writes 0 at
// map load and derives slots by 844-stride arithmetic from the work base; a
// larger value means the scope words are not a live map table, and the call
// declines per-call rather than building from a garbage slot).
constexpr uint32_t kMaxSlotIdx = 1024;
// The build's table bounds (design s4.3/s4.4): a slot physically holds at
// most 2 groups; the index serves tables of at most kMaxEntries entries.
constexpr uint32_t kMaxGroups = 2;
constexpr uint32_t kMaxEntries = 4096;
// Name read cap, query and table side alike: a name with no NUL within the
// cap is not indexable (decline the call / fail the build). Map names are
// short; 256 B is far beyond any real one.
constexpr uint32_t kNameCap = 256;
// Hash table: kTableSlots slots (power of two), linear probing. 8192 slots
// x 16 B = 128 KB; the name arena is 160 KB; both static, rebuilt in place:
// 288 KB hard-capped, ~27 KB live in the known scopes (207-275 entries).
constexpr uint32_t kTableSlots = 8192;
constexpr uint32_t kArenaBytes = 160 * 1024;
// Churn guard (design s4.3): evaluated when a rebuild is requested - if the
// current window already holds a real sample of calls (kChurnMinCalls), at
// least kChurnMinRebuilds rebuilds, and the rebuilds exceed 1% of the
// window's calls, the scope latches DECLINE-ALL until it changes. The
// call-count floor keeps load/transition bursts (few calls, several
// legitimate mutations) from tripping it; the guard is for sustained
// churn, not transients. A churn scene degrades to baseline, never below
// it.
constexpr uint64_t kChurnMinCalls = 2000;
constexpr uint64_t kChurnMinRebuilds = 8;

constexpr uint32_t kSlotStride = 844;
constexpr uint32_t kGroupStride = 420;
constexpr uint32_t kEntryStride = 276;
constexpr uint32_t kSlotIndexOffset = 1688; // workBase+1688: the current slot index
constexpr uint32_t kGroupCountOffset = 392; // group+392: the entry count
constexpr uint32_t kGroupBaseOffset = 396;  // group+396: the entries base pointer
constexpr int32_t kSdaWorkOffset = -32528;  // r13-32528: the map work pointer slot
constexpr uint32_t kSkipBit = 0x80;         // u16 at entry+0: scan-invisible

std::atomic<bool> g_hitIndexOn{false};
std::atomic<bool> g_hitShadowOn{false};

// ---- the index state -----------------------------------------------------------
// One build at a time, for one scope, stamped with the generation it was
// built under. The scope is the triple (workBase, slotIdx, groupCount):
// the slot+0 group count is a scope word too (census s5.4 hardening -
// its writers sit in the mapLoad chain, and reading it per call makes
// its coverage a construction property instead of a census claim; the
// scan reads it on every call as well). Slots hold the first entry (in
// scan order) carrying each name; insert-if-absent at build is what
// preserves first-match semantics for duplicate names exactly.
struct Slot {
    uint32_t hash = 0;
    uint32_t entryAddr = 0; // 0 = empty (a real entry address is never 0)
    uint32_t nameOff = 0;   // offset into g_arena
    uint32_t nameLen = 0;   // name length excluding the NUL
};

Slot g_slots[kTableSlots];
char g_arena[kArenaBytes];
uint32_t g_arenaUsed = 0;
uint32_t g_indexCount = 0;

bool g_builtValid = false;
uint32_t g_builtWorkBase = 0;
uint32_t g_builtSlotIdx = 0;
int32_t g_builtGroupCount = 0;
uint64_t g_builtGen = 0;

// A failed build is sticky for its (scope, generation): the same call
// declines without re-walking until a writer bump or a scope change gives
// the table a new stamp. Without this, an unindexable scope would pay a
// full failed walk per call - the s77 failure churn, designed out here.
bool g_failedValid = false;
uint32_t g_failedWorkBase = 0;
uint32_t g_failedSlotIdx = 0;
int32_t g_failedGroupCount = 0;
uint64_t g_failedGen = 0;

// The churn latch, keyed to the scope that tripped it. It clears when a
// DIFFERENT scope builds successfully (see Rebuild): the churn was a
// property of that scope's residency window, so leaving and returning
// earns a fresh trial window; a scope that is still churning re-latches.
// A latched scope always degrades to the original body, never below it.
// The guard evaluates rebuild/call DELTAS against the baselines below,
// snapshotted whenever a latch clears (and zeroed whenever PrintWindow
// resets the window counters), so a fresh trial starts with fresh
// accounting rather than the churn window's history.
bool g_latched = false;
uint32_t g_latchWorkBase = 0;
uint32_t g_latchSlotIdx = 0;
int32_t g_latchGroupCount = 0;
uint64_t g_churnBaseCalls = 0;
uint64_t g_churnBaseRebuilds = 0;

// The single global generation. Every writer hook bumps it; a build is
// servable only while its stamped generation equals it. Never reset.
std::atomic<uint64_t> g_generation{0};

// ---- counters (windowed like DlProbe's, reset at each print) -------------------
struct Counters {
    uint64_t calls = 0;
    uint64_t servedHits = 0;
    uint64_t servedMisses = 0;
    uint64_t declineOffthread = 0;
    uint64_t declineInsane = 0;
    uint64_t declineUnindexable = 0;
    uint64_t declineOvercap = 0;
    uint64_t declineChurn = 0;
    uint64_t rebuildsScope = 0;
    uint64_t rebuildsGeneration = 0;
    uint64_t failedBuilds = 0;
    uint64_t churnLatches = 0;
    uint64_t bumpsStructure = 0;
    uint64_t bumpsFill = 0;
    uint64_t bumpsDelete = 0;
    uint64_t bumpsFlags = 0;
    uint64_t shadowComparisons = 0;
    uint64_t shadowMismatches = 0;
    // Realised serve-path cost: nanoseconds accumulated inside the answer
    // computation on answered calls only. A call that itself triggers a
    // rebuild pays the rebuild inside its own timing; in standing
    // segments rebuilds are ~absent, so the window average is the serve
    // cost (small windows with several rebuilds read high - the basic
    // host-differential window is exactly that case).
    uint64_t serveNs = 0;
};
Counters g_win;
uint64_t g_mismatchTotal = 0; // cumulative: the first-16 detail lines gate on this

// ---- thread guard ---------------------------------------------------------------
std::once_flag g_ownerOnce;
std::thread::id g_owner{};
std::atomic<bool> g_ownerSet{false};

bool OnOwnerThread(bool record) {
    const std::thread::id id = std::this_thread::get_id();
    if (record) {
        std::call_once(g_ownerOnce, [&] {
            g_owner = id;
            g_ownerSet.store(true, std::memory_order_release);
        });
        return id == g_owner;
    }
    if (!g_ownerSet.load(std::memory_order_acquire)) return false;
    return id == g_owner;
}

// ---- name reads -------------------------------------------------------------------
// Bounded read of a guest NUL-terminated string into out (cap bytes,
// including the NUL). On success *outLen is the length excluding the NUL.
// Returns false when no NUL appears within the cap.
bool ReadName(uint32_t addr, char* out, uint32_t cap, uint32_t* outLen) {
    for (uint32_t i = 0; i < cap; ++i) {
        const char c = static_cast<char>(MemoryInline::FlatRead8(addr + i));
        out[i] = c;
        if (c == '\0') {
            *outLen = i;
            return true;
        }
    }
    return false;
}

uint32_t HashName(const char* name, uint32_t len) {
    uint32_t h = 2166136261u; // FNV-1a
    for (uint32_t i = 0; i < len; ++i) {
        h ^= static_cast<uint8_t>(name[i]);
        h *= 16777619u;
    }
    return h;
}

// Probe for a stored name (full byte compare - the hash only picks the
// probe start). Returns the entry address, or 0 when absent. entryAddr 0
// is the empty-slot marker, so a stored answer of "absent" and a stored
// entry can never be confused: entry addresses are never 0.
uint32_t Probe(const char* name, uint32_t len, uint32_t hash) {
    uint32_t idx = hash & (kTableSlots - 1);
    for (uint32_t n = 0; n < kTableSlots; ++n) {
        const Slot& s = g_slots[idx];
        if (s.entryAddr == 0) return 0;
        if (s.hash == hash && s.nameLen == len &&
            std::memcmp(g_arena + s.nameOff, name, len) == 0) {
            return s.entryAddr;
        }
        idx = (idx + 1) & (kTableSlots - 1);
    }
    return 0; // table full of non-matches (cannot happen under kMaxEntries)
}

// Insert-if-absent at build: the first entry in scan order carrying a name
// is the one the scan returns; later duplicates never enter the table.
// Returns false when the name cannot be stored - the arena is full, or
// the probe loop found no empty slot. The caller fails the whole build:
// a silently dropped name would make Probe answer a proven miss for a
// name that IS in the table (a wrong answer, found by adversarial
// review: the entry cap alone does not bound the arena - 4096 entries
// of 255 B names are ~1 MB against the 160 KB arena; ~641 such names
// exhaust it). Unreachable in the known scopes (275 entries, ~70 KB at
// the name cap), but the failure must be loud, never silent.
bool Insert(const char* name, uint32_t len, uint32_t hash, uint32_t entryAddr) {
    uint32_t idx = hash & (kTableSlots - 1);
    for (uint32_t n = 0; n < kTableSlots; ++n) {
        Slot& s = g_slots[idx];
        if (s.entryAddr == 0) {
            if (g_arenaUsed + len + 1 > kArenaBytes) return false;
            std::memcpy(g_arena + g_arenaUsed, name, len);
            g_arena[g_arenaUsed + len] = '\0';
            s.hash = hash;
            s.entryAddr = entryAddr;
            s.nameOff = g_arenaUsed;
            s.nameLen = len;
            g_arenaUsed += len + 1;
            ++g_indexCount;
            return true;
        }
        if (s.hash == hash && s.nameLen == len &&
            std::memcmp(g_arena + s.nameOff, name, len) == 0) {
            return true; // an earlier entry already carries this name
        }
        idx = (idx + 1) & (kTableSlots - 1);
    }
    return false;
}

// ---- the rebuild walk -------------------------------------------------------------
// One full walk in scan order. On success the build stamp is set and true
// is returned; on any unclassifiable state the build fails whole (the
// previous build, if any, is discarded - its stamp no longer matches
// either way) and the failure is stamped sticky for (scope, generation).
bool Rebuild(uint32_t workBase, uint32_t slotIdx, int32_t groupCount, uint64_t gen) {
    for (uint32_t i = 0; i < kTableSlots; ++i) g_slots[i] = Slot{};
    g_arenaUsed = 0;
    g_indexCount = 0;
    g_builtValid = false;

    const uint32_t slot = workBase + slotIdx * kSlotStride;
    bool ok = true;
    if (groupCount < 0 || groupCount > static_cast<int32_t>(kMaxGroups)) ok = false;
    uint32_t total = 0;
    char name[kNameCap];
    for (int32_t g = 0; ok && g < groupCount; ++g) {
        const uint32_t group = slot + static_cast<uint32_t>(g) * kGroupStride;
        const int32_t count = static_cast<int32_t>(MemoryInline::FlatRead32(group + kGroupCountOffset));
        const uint32_t base = MemoryInline::FlatRead32(group + kGroupBaseOffset);
        if (count < 0 || (base == 0 && count > 0)) { ok = false; break; }
        total += static_cast<uint32_t>(count);
        if (total > kMaxEntries) { ok = false; break; }
        for (int32_t e = 0; e < count; ++e) {
            const uint32_t entry = base + static_cast<uint32_t>(e) * kEntryStride;
            if ((MemoryInline::FlatRead16(entry) & kSkipBit) != 0) continue;
            const uint32_t holder = MemoryInline::FlatRead32(entry + 8);
            if (holder == 0) { ok = false; break; }
            const uint32_t namePtr = MemoryInline::FlatRead32(holder);
            if (namePtr == 0) { ok = false; break; }
            uint32_t len = 0;
            if (!ReadName(namePtr, name, kNameCap, &len)) { ok = false; break; }
            if (!Insert(name, len, HashName(name, len), entry)) { ok = false; break; }
        }
    }
    if (!ok) {
        for (uint32_t i = 0; i < kTableSlots; ++i) g_slots[i] = Slot{};
        g_arenaUsed = 0;
        g_indexCount = 0;
        g_failedValid = true;
        g_failedWorkBase = workBase;
        g_failedSlotIdx = slotIdx;
        g_failedGroupCount = groupCount;
        g_failedGen = gen;
        ++g_win.failedBuilds;
        return false;
    }
    g_builtValid = true;
    g_builtWorkBase = workBase;
    g_builtSlotIdx = slotIdx;
    g_builtGroupCount = groupCount;
    g_builtGen = gen;
    g_failedValid = false;
    if (g_latched && (g_latchWorkBase != workBase || g_latchSlotIdx != slotIdx ||
                      g_latchGroupCount != groupCount)) {
        g_latched = false; // a different scope built: the latched one gets a fresh trial
        g_churnBaseCalls = g_win.calls;
        g_churnBaseRebuilds = g_win.rebuildsScope + g_win.rebuildsGeneration;
    }
    return true;
}

// ---- the answer computation ---------------------------------------------------------
// The per-call scope check is three guest reads (work base, slot index,
// group count at slot+0) - the same three reads the body's own prologue
// performs on every call (the mapGetWork inline plus the scan's head).
enum class Outcome : uint8_t {
    Answered,
    Offthread,
    Insane,
    Unindexable,
    Overcap,
    Churn,
};

// Computes the scan's answer for (ctx, namePtr) from the index. On
// Outcome::Answered, *outAnswer is the first matching entry's address or 0
// (a proven miss, or the null-name early-out). Any other outcome declines:
// the original body serves the call.
Outcome Compute(CpuContext* ctx, uint32_t namePtr, uint32_t* outAnswer) {
    if (!OnOwnerThread(true)) return Outcome::Offthread;
    // The body's early-out: a null name pointer returns 0 without touching
    // the table, so it is answered before any scope read.
    if (namePtr == 0) {
        *outAnswer = 0;
        return Outcome::Answered;
    }
    const uint32_t gpr13 = static_cast<uint32_t>(ctx->gpr[13]);
    const uint32_t workBase = MemoryInline::FlatRead32(gpr13 + static_cast<uint32_t>(kSdaWorkOffset));
    if (workBase == 0) return Outcome::Insane;
    const uint32_t slotIdx = MemoryInline::FlatRead32(workBase + kSlotIndexOffset);
    if (slotIdx > kMaxSlotIdx) return Outcome::Insane;
    // The third scope word (census s5.4): the group count at slot+0.
    const int32_t groupCount = static_cast<int32_t>(
        MemoryInline::FlatRead32(workBase + slotIdx * kSlotStride));
    const uint64_t gen = g_generation.load(std::memory_order_relaxed);

    if (g_latched && g_latchWorkBase == workBase && g_latchSlotIdx == slotIdx &&
        g_latchGroupCount == groupCount) {
        return Outcome::Churn;
    }
    const bool stampMatches = g_builtValid && g_builtWorkBase == workBase &&
                              g_builtSlotIdx == slotIdx && g_builtGroupCount == groupCount &&
                              g_builtGen == gen;
    if (!stampMatches) {
        if (g_failedValid && g_failedWorkBase == workBase && g_failedSlotIdx == slotIdx &&
            g_failedGroupCount == groupCount && g_failedGen == gen) {
            return Outcome::Unindexable;
        }
        const bool sameScope = g_builtValid && g_builtWorkBase == workBase &&
                               g_builtSlotIdx == slotIdx && g_builtGroupCount == groupCount;
        if (sameScope) {
            // Churn guard, evaluated before paying for another
            // generation rebuild of the resident scope, on deltas since
            // the last latch clear (see the latch state above). Scope
            // rebuilds are never guarded: a scope switch is a residency
            // change (a load), not churn, and it is also the latch's
            // escape hatch.
            const uint64_t rebuilds =
                (g_win.rebuildsScope + g_win.rebuildsGeneration) - g_churnBaseRebuilds;
            const uint64_t calls = g_win.calls - g_churnBaseCalls;
            if (calls >= kChurnMinCalls && rebuilds >= kChurnMinRebuilds &&
                rebuilds * 100 > calls) {
                g_latched = true;
                g_latchWorkBase = workBase;
                g_latchSlotIdx = slotIdx;
                g_latchGroupCount = groupCount;
                ++g_win.churnLatches;
                return Outcome::Churn;
            }
            ++g_win.rebuildsGeneration;
        } else {
            ++g_win.rebuildsScope;
        }
        if (!Rebuild(workBase, slotIdx, groupCount, gen)) return Outcome::Unindexable;
    }

    char query[kNameCap];
    uint32_t qlen = 0;
    if (!ReadName(namePtr, query, kNameCap, &qlen)) return Outcome::Overcap;
    *outAnswer = Probe(query, qlen, HashName(query, qlen));
    return Outcome::Answered;
}

void CountOutcome(Outcome o) {
    switch (o) {
        case Outcome::Offthread: ++g_win.declineOffthread; break;
        case Outcome::Insane: ++g_win.declineInsane; break;
        case Outcome::Unindexable: ++g_win.declineUnindexable; break;
        case Outcome::Overcap: ++g_win.declineOvercap; break;
        case Outcome::Churn: ++g_win.declineChurn; break;
        case Outcome::Answered: break;
    }
}

// ---- the per-call mode latch ---------------------------------------------------------
// HitIndexCallBegin latches, at entry, that it ran for this call and which
// mode was on. The return block consults ONLY this latch (through
// HitIndexCallBegun / HitIndexCallShadow, closed by HitIndexCallEnd) and
// never re-reads the diag flags, so a live flag flip landing mid-scan takes
// effect from the next call's entry (bbox code review F1).
bool g_callBegun = false;
bool g_callShadow = false;

// ---- the shadow stash ------------------------------------------------------------------
uint64_t g_callSeq = 0; // bumped by HitIndexCallBegin on every call while a mode is on
uint64_t g_curSeq = 0;  // the sequence value of the call currently inside the body
struct Stash {
    bool valid = false;
    uint64_t stamp = 0;
    uint32_t answer = 0;
    uint32_t namePtr = 0;
    uint32_t workBase = 0;
    uint32_t slotIdx = 0;
    char query[kNameCap] = {};
};
Stash g_stash;

} // namespace

bool Enabled() noexcept { return g_hitIndexOn.load(std::memory_order_relaxed); }

bool ShadowEnabled() noexcept { return g_hitShadowOn.load(std::memory_order_relaxed); }

void PrintWindow(uint64_t firstFrame, uint64_t lastFrame) {
    // Bumps alone do not open a window line: the writer hooks fire in both
    // A/B arms (and with both keys off), and the key-off control must stay
    // quiet. A suppressed window's counters are NOT reset (bbox review F5):
    // in a suppressed window the only nonzero counters can be the bump
    // counts, and those carry into the next printed window so the
    // dirty-event census does not silently lose bump-only windows.
    const bool anyActivity = g_win.calls != 0 || g_win.servedHits != 0 || g_win.servedMisses != 0 ||
                             g_win.declineOffthread != 0 || g_win.declineInsane != 0 ||
                             g_win.declineUnindexable != 0 || g_win.declineOvercap != 0 ||
                             g_win.declineChurn != 0 || g_win.rebuildsScope != 0 ||
                             g_win.rebuildsGeneration != 0 || g_win.failedBuilds != 0 ||
                             g_win.churnLatches != 0 || g_win.shadowComparisons != 0 ||
                             g_win.shadowMismatches != 0;
    if (!Enabled() && !ShadowEnabled() && !anyActivity) return;
    const Counters win = g_win;
    g_win = Counters{};
    g_churnBaseCalls = 0;
    g_churnBaseRebuilds = 0;
    const uint64_t answered = win.servedHits + win.servedMisses;
    const uint64_t serveAvgNs = answered != 0 ? win.serveNs / answered : 0;
    std::fprintf(stderr,
                 "[hitindex] frames %llu-%llu: %llu calls, %llu hits, %llu misses, "
                 "declines offthread %llu insane %llu unindexable %llu overcap %llu churn %llu, "
                 "rebuilds scope %llu generation %llu, failed builds %llu, churn latches %llu, "
                 "bumps structure %llu fill %llu delete %llu flags %llu, "
                 "shadow %llu comparisons %llu mismatches, index %u entries, "
                 "serve avg %llu ns/answer\n",
                 static_cast<unsigned long long>(firstFrame),
                 static_cast<unsigned long long>(lastFrame),
                 static_cast<unsigned long long>(win.calls),
                 static_cast<unsigned long long>(win.servedHits),
                 static_cast<unsigned long long>(win.servedMisses),
                 static_cast<unsigned long long>(win.declineOffthread),
                 static_cast<unsigned long long>(win.declineInsane),
                 static_cast<unsigned long long>(win.declineUnindexable),
                 static_cast<unsigned long long>(win.declineOvercap),
                 static_cast<unsigned long long>(win.declineChurn),
                 static_cast<unsigned long long>(win.rebuildsScope),
                 static_cast<unsigned long long>(win.rebuildsGeneration),
                 static_cast<unsigned long long>(win.failedBuilds),
                 static_cast<unsigned long long>(win.churnLatches),
                 static_cast<unsigned long long>(win.bumpsStructure),
                 static_cast<unsigned long long>(win.bumpsFill),
                 static_cast<unsigned long long>(win.bumpsDelete),
                 static_cast<unsigned long long>(win.bumpsFlags),
                 static_cast<unsigned long long>(win.shadowComparisons),
                 static_cast<unsigned long long>(win.shadowMismatches),
                 g_indexCount,
                 static_cast<unsigned long long>(serveAvgNs));
}

} // namespace HitIndex

extern "C" void spm_hitindex_set(bool enabled) {
    HitIndex::g_hitIndexOn.store(enabled, std::memory_order_relaxed);
}

extern "C" void spm_hitindexshadow_set(bool enabled) {
    HitIndex::g_hitShadowOn.store(enabled, std::memory_order_relaxed);
}

extern "C" uint64_t HitIndexCallBegin() {
    ++HitIndex::g_callSeq;
    HitIndex::g_curSeq = HitIndex::g_callSeq;
    ++HitIndex::g_win.calls;
    // Latch this call's mode at entry (bbox review F1). Any earlier stash
    // was consumed (or discarded as stamp-foreign) by its own call's return
    // block; clear the slot so none can outlive its call.
    HitIndex::g_callBegun = true;
    HitIndex::g_callShadow = HitIndex::ShadowEnabled();
    HitIndex::g_stash.valid = false;
    return HitIndex::g_curSeq;
}

extern "C" bool HitIndexCallBegun() { return HitIndex::g_callBegun; }

extern "C" bool HitIndexCallShadow() {
    return HitIndex::g_callBegun && HitIndex::g_callShadow;
}

extern "C" void HitIndexCallEnd() {
    HitIndex::g_callBegun = false;
    HitIndex::g_callShadow = false;
}

extern "C" bool HitIndexEnabled() { return HitIndex::Enabled(); }

extern "C" bool HitIndexShadowEnabled() { return HitIndex::ShadowEnabled(); }

extern "C" bool HitIndexTry(CpuContext* ctx, uint32_t namePtr, uint32_t* outResult) {
    using namespace HitIndex;
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t answer = 0;
    const Outcome o = Compute(ctx, namePtr, &answer);
    if (o != Outcome::Answered) {
        CountOutcome(o);
        return false;
    }
    *outResult = answer;
    if (answer != 0) {
        ++g_win.servedHits;
    } else {
        ++g_win.servedMisses;
    }
    const auto t1 = std::chrono::steady_clock::now();
    g_win.serveNs += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return true;
}

extern "C" void HitIndexShadowNote(CpuContext* ctx, uint32_t namePtr) {
    using namespace HitIndex;
    uint32_t answer = 0;
    const Outcome o = Compute(ctx, namePtr, &answer);
    if (o != Outcome::Answered) {
        CountOutcome(o);
        return; // a decline stashes nothing
    }
    Stash& stash = g_stash;
    stash.valid = true;
    stash.stamp = g_curSeq;
    stash.answer = answer;
    stash.namePtr = namePtr;
    // For the mismatch detail line: the scope words and the query content,
    // re-read here (the note path is proof machinery, not the serve path).
    const uint32_t gpr13 = static_cast<uint32_t>(ctx->gpr[13]);
    stash.workBase = MemoryInline::FlatRead32(gpr13 + static_cast<uint32_t>(kSdaWorkOffset));
    stash.slotIdx = stash.workBase != 0
                        ? MemoryInline::FlatRead32(stash.workBase + kSlotIndexOffset)
                        : 0;
    stash.query[0] = '\0';
    if (namePtr != 0) {
        uint32_t len = 0;
        if (ReadName(namePtr, stash.query, kNameCap, &len)) {
            // query holds the name; len already bounded by the cap
        } else {
            stash.query[0] = '\0';
        }
    }
    if (answer != 0) {
        ++g_win.servedHits; // shadow mode: hits/misses count would-be answers
    } else {
        ++g_win.servedMisses;
    }
}

extern "C" void HitIndexShadowCompare(uint32_t scanResult) {
    using namespace HitIndex;
    if (!g_callBegun || !g_callShadow) return; // latched-mode gate (F1)
    if (!OnOwnerThread(false)) return;
    Stash& stash = g_stash;
    if (!stash.valid) return;
    if (stash.stamp != g_curSeq) {
        // Stale or foreign: discard it and count nothing.
        stash.valid = false;
        return;
    }
    const Stash mine = stash;
    stash.valid = false; // consume
    ++g_win.shadowComparisons;
    if (mine.answer != scanResult) {
        ++g_win.shadowMismatches;
        ++g_mismatchTotal;
        if (g_mismatchTotal <= 16) {
            std::fprintf(stderr,
                         "[hitindex] mismatch %llu: query \"%s\" (ptr 0x%08X) scope workBase "
                         "0x%08X slot %u: scan 0x%08X vs index 0x%08X\n",
                         static_cast<unsigned long long>(g_mismatchTotal), mine.query,
                         mine.namePtr, mine.workBase, mine.slotIdx, scanResult, mine.answer);
        }
    }
}

extern "C" void HitIndexBumpStructure() {
    HitIndex::g_generation.fetch_add(1, std::memory_order_relaxed);
    ++HitIndex::g_win.bumpsStructure;
}

extern "C" void HitIndexBumpFill() {
    HitIndex::g_generation.fetch_add(1, std::memory_order_relaxed);
    ++HitIndex::g_win.bumpsFill;
}

extern "C" void HitIndexBumpDelete() {
    HitIndex::g_generation.fetch_add(1, std::memory_order_relaxed);
    ++HitIndex::g_win.bumpsDelete;
}

extern "C" void HitIndexBumpFlags() {
    HitIndex::g_generation.fetch_add(1, std::memory_order_relaxed);
    ++HitIndex::g_win.bumpsFlags;
}
