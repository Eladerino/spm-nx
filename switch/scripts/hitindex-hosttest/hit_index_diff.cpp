// Host differential for the hit index helper (hit_index.cpp): the index
// vs an independent reference implementation of the original scan, over
// synthetic guest tables in the stub flat memory (see stub/memory_access.h).
//
// The harness drives the helper exactly the way the patched body does:
// HitIndexCallBegin at entry (mode latched), HitIndexTry /
// HitIndexShadowNote inside, HitIndexShadowCompare at the return block,
// HitIndexCallEnd to close. Mutations are applied to the stub memory and
// paired with the bump entry point the corresponding patched writer body
// would call - the same discipline the real table's writers follow.
//
// Phases (argv[1]): basic | shadow | corrupt | churn | arena. Each runs in a
// fresh process so the helper's state starts clean. Every phase prints
// "HARNESS PASS" or "HARNESS FAIL <n>" as its last stdout line; window
// lines ([hitindex] ...) go to stderr via PrintWindow for the wrapper to
// check (shadow comparisons / mismatches).

#include "hit_index.h"

#include "memory_access.h"
#include "ppc_runtime.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

// ---- guest image layout -----------------------------------------------------
constexpr uint32_t kSdaBase = 0x80008000;
constexpr uint32_t kWorkPtrAddr = kSdaBase - 32528; // the SDA map-work slot
constexpr uint32_t kWorkBase = 0x80010000;          // slot 0 at +0, slot 1 at +844
constexpr uint32_t kG0Base = 0x80A00000;            // slot 0 group 0 entries
constexpr uint32_t kG1Base = 0x80B00000;            // slot 0 group 1 entries
constexpr uint32_t kAltBase = 0x80C00000;           // alternate table (reinit target)
constexpr uint32_t kHolderBase = 0x80200000;        // holders, 64 B apart
constexpr uint32_t kNameBase = 0x80300000;          // name strings, 128 B apart
constexpr uint32_t kQueryBuf = 0x80400000;
constexpr uint32_t kQueryBuf2 = 0x80410000;         // same content, other pointer

CpuContext g_ctx;
int g_failures = 0;
int g_checks = 0;

void Expect(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// ---- guest writes -------------------------------------------------------------
void PutStr(uint32_t addr, const char* s) {
    uint32_t i = 0;
    for (; s[i] != '\0'; ++i) MemoryInline::FlatWrite8(addr + i, static_cast<uint8_t>(s[i]));
    MemoryInline::FlatWrite8(addr + i, 0);
}

uint32_t NameAddr(int i) { return kNameBase + static_cast<uint32_t>(i) * 128; }
uint32_t MakeName(int i, const char* s) {
    PutStr(NameAddr(i), s);
    return NameAddr(i);
}
uint32_t HolderAddr(int i) { return kHolderBase + static_cast<uint32_t>(i) * 64; }
uint32_t MakeHolder(int i, uint32_t nameAddr) {
    MemoryInline::FlatWrite32(HolderAddr(i), nameAddr);
    return HolderAddr(i);
}

void SetGroup(uint32_t slotBase, int g, int32_t count, uint32_t base) {
    MemoryInline::FlatWrite32(slotBase + static_cast<uint32_t>(g) * 420 + 392,
                              static_cast<uint32_t>(count));
    MemoryInline::FlatWrite32(slotBase + static_cast<uint32_t>(g) * 420 + 396, base);
}
void SetEntry(uint32_t base, int i, uint16_t flags, uint32_t holder) {
    const uint32_t e = base + static_cast<uint32_t>(i) * 276;
    MemoryInline::FlatWrite16(e, flags);
    MemoryInline::FlatWrite32(e + 8, holder);
}
void FillSkipped(uint32_t base, int count) {
    for (int i = 0; i < count; ++i) SetEntry(base, i, 0x80, 0);
}
uint32_t EntryAddr(uint32_t base, int i) { return base + static_cast<uint32_t>(i) * 276; }

void ResetImage() {
    std::memset(TestMem::g_mem, 0, sizeof(TestMem::g_mem));
    MemoryInline::FlatWrite32(kWorkPtrAddr, kWorkBase);
    g_ctx = CpuContext{};
    g_ctx.gpr[13] = kSdaBase;
    MemoryInline::FlatWrite32(kWorkBase + 1688, 0); // slot index 0
}

// The main synthetic table: the insanity diagnosis's pinned image -
// groupCount 2 at slot+0, counts 300/250 at slot+392/slot+812, entries
// bases 0x80xxxxxx at slot+396/slot+816.
void BuildMainTable() {
    ResetImage();
    const uint32_t slot = kWorkBase;
    MemoryInline::FlatWrite32(slot, 2); // group count at slot+0
    SetGroup(slot, 0, 300, kG0Base);
    SetGroup(slot, 1, 250, kG1Base);
    FillSkipped(kG0Base, 300);
    FillSkipped(kG1Base, 250);
    SetEntry(kG0Base, 5, 0, MakeHolder(0, MakeName(0, "alpha")));
    SetEntry(kG0Base, 100, 0, MakeHolder(1, MakeName(1, "beta")));
    SetEntry(kG0Base, 150, 0, MakeHolder(2, MakeName(2, "alpha"))); // duplicate, later
    SetEntry(kG0Base, 200, 0x80, MakeHolder(6, MakeName(6, "omega"))); // skipped for now
    SetEntry(kG1Base, 0, 0, MakeHolder(3, MakeName(3, "gamma")));
    SetEntry(kG1Base, 7, 0, MakeHolder(4, MakeName(4, "beta"))); // duplicate, group 1
    SetEntry(kG1Base, 249, 0, MakeHolder(5, MakeName(5, "delta")));
}

// ---- the reference scan (independent implementation of the body's walk) -------
bool GuestStrEq(uint32_t a, uint32_t b) {
    for (uint32_t i = 0;; ++i) {
        const uint8_t ca = MemoryInline::FlatRead8(a + i);
        const uint8_t cb = MemoryInline::FlatRead8(b + i);
        if (ca != cb) return false;
        if (ca == 0) return true;
    }
}

uint32_t RefScan(uint32_t namePtr) {
    if (namePtr == 0) return 0;
    const uint32_t workBase = MemoryInline::FlatRead32(
        static_cast<uint32_t>(static_cast<int32_t>(g_ctx.gpr[13]) - 32528));
    const uint32_t slotIdx = MemoryInline::FlatRead32(workBase + 1688);
    const uint32_t slot = workBase + slotIdx * 844;
    const int32_t groupCount = static_cast<int32_t>(MemoryInline::FlatRead32(slot));
    for (int32_t g = 0; g < groupCount; ++g) {
        const uint32_t group = slot + static_cast<uint32_t>(g) * 420;
        const int32_t count = static_cast<int32_t>(MemoryInline::FlatRead32(group + 392));
        const uint32_t base = MemoryInline::FlatRead32(group + 396);
        for (int32_t e = 0; e < count; ++e) {
            const uint32_t entry = base + static_cast<uint32_t>(e) * 276;
            if ((MemoryInline::FlatRead16(entry) & 0x80) != 0) continue;
            const uint32_t holder = MemoryInline::FlatRead32(entry + 8);
            const uint32_t np = MemoryInline::FlatRead32(holder);
            if (GuestStrEq(np, namePtr)) return entry;
        }
    }
    return 0;
}

// ---- call plumbing (mirrors the patched body's blocks) -------------------------
uint32_t WriteQuery(uint32_t buf, const char* s) {
    PutStr(buf, s);
    return buf;
}

bool TryQuery(uint32_t namePtr, uint32_t& ans) {
    HitIndexCallBegin();
    bool ok = false;
    if (HitIndexEnabled()) ok = HitIndexTry(&g_ctx, namePtr, &ans);
    HitIndexCallEnd();
    return ok;
}

void CheckQuery(const char* name, const char* what) {
    const uint32_t qp = WriteQuery(kQueryBuf, name);
    uint32_t ans = 0xDEADBEEF;
    const bool ok = TryQuery(qp, ans);
    Expect(ok, what);
    Expect(ans == RefScan(qp), what);
}

void CheckDeclines(const char* name, const char* what) {
    const uint32_t qp = WriteQuery(kQueryBuf, name);
    uint32_t ans = 0;
    Expect(!TryQuery(qp, ans), what);
}

void ShadowQuery(uint32_t namePtr) {
    HitIndexCallBegin();
    HitIndexShadowNote(&g_ctx, namePtr);
    const uint32_t scan = RefScan(namePtr);
    HitIndexShadowCompare(scan);
    HitIndexCallEnd();
}

int Finish() {
    std::printf("%s (%d checks, %d failures)\n", g_failures == 0 ? "HARNESS PASS" : "HARNESS FAIL",
                g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// ---- phase: basic ---------------------------------------------------------------
int PhaseBasic() {
    spm_hitindex_set(true);
    BuildMainTable();

    // First-match semantics over the pinned image, hits and misses.
    CheckQuery("alpha", "alpha -> group0 idx5");
    {
        const uint32_t qp = WriteQuery(kQueryBuf, "alpha");
        uint32_t ans = 0;
        Expect(TryQuery(qp, ans) && ans == EntryAddr(kG0Base, 5), "alpha address exact");
    }
    CheckQuery("beta", "beta -> group0 idx100 (not the group1 duplicate)");
    CheckQuery("gamma", "gamma -> group1 idx0");
    CheckQuery("delta", "delta -> group1 idx249");
    CheckQuery("missing", "missing -> 0");
    CheckQuery("omega", "omega skipped (0x80) -> 0");
    CheckQuery("", "empty query -> 0");
    // Content keying: the same content through a different guest pointer.
    {
        const uint32_t qp = WriteQuery(kQueryBuf2, "alpha");
        uint32_t ans = 0;
        Expect(TryQuery(qp, ans) && ans == EntryAddr(kG0Base, 5),
               "alpha via a second pointer -> same entry");
    }
    // Null name pointer: the body's early-out, answered 0.
    {
        uint32_t ans = 0xDEADBEEF;
        Expect(TryQuery(0, ans) && ans == 0, "null name -> answered 0");
    }

    // Mutation: hitDelete on the first alpha (holder zeroed, 0x80 set).
    SetEntry(kG0Base, 5, 0x80, 0);
    HitIndexBumpDelete();
    CheckQuery("alpha", "after delete, alpha -> the idx150 duplicate");
    {
        const uint32_t qp = WriteQuery(kQueryBuf, "alpha");
        uint32_t ans = 0;
        Expect(TryQuery(qp, ans) && ans == EntryAddr(kG0Base, 150), "promoted duplicate exact");
    }

    // Mutation: flag worker clears omega's 0x80 bit.
    SetEntry(kG0Base, 200, 0, HolderAddr(6));
    HitIndexBumpFlags();
    CheckQuery("omega", "after flag clear, omega answers");

    // Mutation, census amendment H8/H9: the Obj flag wrappers never call
    // the workers - they write the found entry's +0 DIRECTLY with their
    // own mask. Simulated exactly that way on delta (group1 idx249).
    {
        const uint32_t e = EntryAddr(kG1Base, 249);
        MemoryInline::FlatWrite16(e, static_cast<uint16_t>(MemoryInline::FlatRead16(e) | 0x80));
        HitIndexBumpFlags();
        CheckQuery("delta", "wrapper-direct 0x80 set: delta misses");
        MemoryInline::FlatWrite16(e, static_cast<uint16_t>(MemoryInline::FlatRead16(e) & ~0x80));
        HitIndexBumpFlags();
        CheckQuery("delta", "wrapper-direct 0x80 clear: delta answers");
    }

    // Mutation: _hitEnt reuses beta's slot for a new name.
    SetEntry(kG0Base, 100, 0, MakeHolder(7, MakeName(7, "zeta")));
    HitIndexBumpFill();
    CheckQuery("zeta", "reused slot -> zeta at group0 idx100");
    CheckQuery("beta", "beta now resolves to the group1 duplicate");

    // Mutation: hitReinit points the slot at a different table.
    FillSkipped(kAltBase, 4);
    SetEntry(kAltBase, 0, 0, MakeHolder(8, MakeName(8, "uno")));
    SetEntry(kAltBase, 1, 0, MakeHolder(9, MakeName(9, "dos")));
    SetGroup(kWorkBase, 0, 4, kAltBase);
    SetGroup(kWorkBase, 1, 0, 0);
    HitIndexBumpStructure();
    CheckQuery("uno", "after reinit, uno answers");
    CheckQuery("alpha", "after reinit, alpha misses");

    // Scope switch (no bump - the per-call scope check covers it) and back.
    {
        const uint32_t slot1 = kWorkBase + 844;
        MemoryInline::FlatWrite32(slot1, 1);
        SetGroup(slot1, 0, 250, kG1Base);
        MemoryInline::FlatWrite32(kWorkBase + 1688, 1);
        CheckQuery("gamma", "slot 1: gamma answers from its own table");
        CheckQuery("uno", "slot 1: uno misses");
        MemoryInline::FlatWrite32(kWorkBase + 1688, 0);
        CheckQuery("uno", "back in slot 0: uno answers again");
    }

    // Hardening (census s5.4): slot+0 is part of the scope tuple, so a
    // group-count change with NO bump at all is still caught by the
    // per-call scope check. Slot 0 currently has groups (4, alt) and
    // (0, none) with the count word 2.
    {
        SetGroup(kWorkBase, 1, 250, kG1Base); // gamma's table becomes group 1
        HitIndexBumpStructure();
        CheckQuery("gamma", "group 1 live: gamma answers");
        MemoryInline::FlatWrite32(kWorkBase, 1); // slot+0: 2 -> 1, NO bump
        CheckQuery("gamma", "unbumped slot+0 shrink: gamma misses (scope rebuilt)");
        CheckQuery("uno", "unbumped slot+0 shrink: uno still answers");
        MemoryInline::FlatWrite32(kWorkBase, 2); // restore, NO bump
        CheckQuery("gamma", "unbumped slot+0 restore: gamma answers again");
    }

    // Unclassifiable state: an in-count entry with flag clear, holder 0.
    SetEntry(kAltBase, 2, 0, 0);
    SetGroup(kWorkBase, 0, 4, kAltBase); // (count already 4; restated for clarity)
    HitIndexBumpStructure();
    CheckDeclines("uno", "unclassifiable scope declines (hit)");
    CheckDeclines("missing", "unclassifiable scope declines (miss)");
    CheckDeclines("dos", "unclassifiable scope declines (every call)");
    // Repair the entry (skip it) and the scope serves again.
    SetEntry(kAltBase, 2, 0x80, 0);
    HitIndexBumpStructure();
    CheckQuery("uno", "repaired scope answers again");

    // Over-cap table: a group count beyond the entry cap fails the build.
    SetGroup(kWorkBase, 0, 5000, kG0Base);
    HitIndexBumpStructure();
    CheckDeclines("uno", "over-cap table declines");
    SetGroup(kWorkBase, 0, 4, kAltBase);
    HitIndexBumpStructure();
    CheckQuery("uno", "restored table answers");

    // Over-long query (no NUL within the 256 B cap) declines.
    {
        char big[320];
        std::memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        const uint32_t qp = WriteQuery(kQueryBuf, big);
        uint32_t ans = 0;
        Expect(!TryQuery(qp, ans), "over-long query declines");
    }

    HitIndex::PrintWindow(0, 450);
    return Finish();
}

// ---- phase: shadow ----------------------------------------------------------------
int PhaseShadow() {
    spm_hitindexshadow_set(true);
    BuildMainTable();
    int issued = 0;
    auto q = [&](const char* name) {
        ShadowQuery(WriteQuery(kQueryBuf, name));
        ++issued;
    };
    for (int i = 0; i < 10; ++i) q("alpha");
    for (int i = 0; i < 10; ++i) q("missing");
    q("beta");
    q("gamma");
    q("delta");
    ShadowQuery(0); // null name: answered 0, compared
    ++issued;
    // A mutation WITH its bump, mid-stream: the index must track it.
    SetEntry(kG0Base, 5, 0x80, 0);
    HitIndexBumpDelete();
    for (int i = 0; i < 8; ++i) q("alpha"); // now the idx150 duplicate
    SetEntry(kG0Base, 100, 0, MakeHolder(7, MakeName(7, "zeta")));
    HitIndexBumpFill();
    for (int i = 0; i < 8; ++i) q("zeta");
    std::printf("shadow queries issued: %d (expect comparisons == issued, mismatches == 0)\n", issued);
    HitIndex::PrintWindow(0, 450);
    return Finish();
}

// ---- phase: corrupt (the shadow net must catch an unbumped mutation) ---------------
int PhaseCorrupt() {
    spm_hitindexshadow_set(true);
    BuildMainTable();
    for (int i = 0; i < 5; ++i) ShadowQuery(WriteQuery(kQueryBuf, "alpha"));
    // A mutation with NO bump - the census-hole simulation. The holder of
    // the first alpha now names a different string, so the scan promotes
    // the idx150 duplicate while the index still serves idx5.
    MemoryInline::FlatWrite32(HolderAddr(0), MakeName(6, "omega"));
    ShadowQuery(WriteQuery(kQueryBuf, "alpha"));
    std::printf("corrupt phase done (expect comparisons == 6, mismatches == 1)\n");
    HitIndex::PrintWindow(0, 450);
    return Finish();
}

// ---- phase: churn -------------------------------------------------------------------
int PhaseChurn() {
    spm_hitindex_set(true);
    ResetImage();
    const uint32_t slot = kWorkBase;
    MemoryInline::FlatWrite32(slot, 1);
    SetGroup(slot, 0, 4, kG0Base);
    FillSkipped(kG0Base, 4);
    SetEntry(kG0Base, 0, 0, MakeHolder(0, MakeName(0, "stable")));
    SetEntry(kG0Base, 1, 0, MakeHolder(1, MakeName(1, "c000")));

    // Build a real call sample on a stable index (the churn guard needs
    // its call-count floor before the share test applies).
    {
        const uint32_t qp = WriteQuery(kQueryBuf, "stable");
        int answered = 0;
        for (int i = 0; i < 2000; ++i) {
            uint32_t ans = 0;
            if (TryQuery(qp, ans) && ans == EntryAddr(kG0Base, 0)) ++answered;
        }
        Expect(answered == 2000, "stable index answers 2000 calls");
    }
    // Sustained churn: rewrite entry idx1's name and bump, every call.
    int answeredCycles = 0;
    int declinedCycles = 0;
    bool seenDecline = false;
    bool declineStuck = true;
    for (int k = 1; k <= 40; ++k) {
        char nm[16];
        std::snprintf(nm, sizeof(nm), "c%03d", k);
        PutStr(NameAddr(1), nm); // same holder, new content (a writer's rewrite)
        HitIndexBumpFill();
        const uint32_t qp = WriteQuery(kQueryBuf, nm);
        uint32_t ans = 0;
        const bool ok = TryQuery(qp, ans);
        if (ok) {
            ++answeredCycles;
            Expect(ans == EntryAddr(kG0Base, 1), "churn cycle answer exact");
            Expect(!seenDecline, "no answer after the latch tripped");
        } else {
            ++declinedCycles;
            seenDecline = true;
        }
        if (seenDecline && ok) declineStuck = false;
    }
    Expect(answeredCycles >= 10 && answeredCycles <= 30, "latch trips mid-churn, not at once");
    Expect(declinedCycles >= 10, "latch holds for the rest of the churn");
    Expect(declineStuck, "once latched, the scope stays declined");

    // A different scope builds successfully (and clears the latch).
    {
        const uint32_t slot1 = kWorkBase + 844;
        MemoryInline::FlatWrite32(slot1, 1);
        SetGroup(slot1, 0, 0, 0);
        MemoryInline::FlatWrite32(kWorkBase + 1688, 1);
        const uint32_t qp = WriteQuery(kQueryBuf, "stable");
        uint32_t ans = 0xDEADBEEF;
        Expect(TryQuery(qp, ans) && ans == 0, "other scope answers (miss) after latch");
        MemoryInline::FlatWrite32(kWorkBase + 1688, 0);
        // Back in the churned scope: a fresh trial - it answers again.
        const uint32_t qp2 = WriteQuery(kQueryBuf, "stable");
        uint32_t ans2 = 0;
        Expect(TryQuery(qp2, ans2) && ans2 == EntryAddr(kG0Base, 0),
               "latched scope earns a fresh trial after a scope round-trip");
    }

    HitIndex::PrintWindow(0, 450);
    return Finish();
}

// ---- phase: arena -------------------------------------------------------------------
// The adversarial reviewer's hole, as a regression: 700 distinct 255 B
// names need ~179 KB of name arena against the 160 KB cap (exhaustion at
// ~641 names) while staying well under the 4096-entry cap. The build must
// fail WHOLE and every query must decline (sticky failure) - a table that
// silently omits tail names would answer proven misses. The reference scan
// DOES find name #660 in this table, which is what makes the shape sharp:
// pre-fix the index served 0 for it; post-fix it declines like everything
// else. Shrinking the scope must let the build recover.
int PhaseArena() {
    spm_hitindex_set(true);
    ResetImage();
    const uint32_t kBigBase = 0x80E00000;   // 700 entries * 276 B
    const uint32_t kBigNames = 0x80D00000;  // 700 names * 300 B stride
    const int kBig = 700;
    char nm[260];
    auto bigName = [&](int i) {
        std::memset(nm, 'n', 253);
        nm[253] = static_cast<char>('a' + (i / 26) % 26);
        nm[254] = static_cast<char>('a' + i % 26);
        nm[255] = '\0';
        return nm;
    };
    const uint32_t slot = kWorkBase;
    MemoryInline::FlatWrite32(slot, 1); // group count at slot+0
    SetGroup(slot, 0, kBig, kBigBase);
    for (int i = 0; i < kBig; ++i) {
        const uint32_t np = kBigNames + static_cast<uint32_t>(i) * 300;
        PutStr(np, bigName(i));
        SetEntry(kBigBase, i, 0, MakeHolder(i, np));
    }
    HitIndexBumpStructure(); // fresh table image

    // The scan finds #660 - the shape is real, not a phantom table.
    {
        const uint32_t qp = WriteQuery(kQueryBuf, bigName(660));
        Expect(RefScan(qp) == EntryAddr(kBigBase, 660), "scan finds name #660");
    }
    // Nothing may be served: the build cannot be honoured, so it must not
    // stand - not even the names that would have fit under the cap.
    for (int i : {0, 100, 640, 660, 699}) {
        const uint32_t qp = WriteQuery(kQueryBuf, bigName(i));
        uint32_t ans = 0xDEADBEEF;
        Expect(!TryQuery(qp, ans), "arena-overflowing scope declines every query");
    }
    // Recovery: shrink to 10 names - the same build now fits and serves.
    SetGroup(slot, 0, 10, kBigBase);
    HitIndexBumpFill();
    {
        const uint32_t qp = WriteQuery(kQueryBuf, bigName(5));
        uint32_t ans = 0;
        Expect(TryQuery(qp, ans) && ans == EntryAddr(kBigBase, 5),
               "shrunk scope serves again");
        Expect(RefScan(qp) == ans, "shrunk scope matches the scan");
    }

    HitIndex::PrintWindow(0, 450);
    return Finish();
}

} // namespace

int main(int argc, char** argv) {
    const char* phase = argc > 1 ? argv[1] : "basic";
    if (std::strcmp(phase, "basic") == 0) return PhaseBasic();
    if (std::strcmp(phase, "shadow") == 0) return PhaseShadow();
    if (std::strcmp(phase, "corrupt") == 0) return PhaseCorrupt();
    if (std::strcmp(phase, "churn") == 0) return PhaseChurn();
    if (std::strcmp(phase, "arena") == 0) return PhaseArena();
    std::printf("unknown phase %s\n", phase);
    return 2;
}
