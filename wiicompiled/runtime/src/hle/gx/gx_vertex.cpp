#include <fstream>
// gx_vertex.cpp - Vertex Descriptor and Attribute Functions
#include "det_clock.h"
#include "gx_internal.h"
#include "gx_stream_common.h"
#include "gx_worker.h"
#include "fiber_manager.h"
#include "guest_interrupt_context.h"
#include "runtime_log.h"

#include <cstdio>
#include <cstdlib>

namespace {
uint32_t CanonicalVtxAttr(uint32_t attr) {
    return attr == GX_VA_NBT ? GX_VA_NRM : attr;
}

// There is deliberately no indexed-aurora path here. The immediate begin path
// streams vertex data incrementally, and aurora's indexed-array upload must
// precede the draw carrying the max-index bounds, which cannot be known without
// buffering the whole primitive. Indexed attributes are therefore expanded into
// the packed direct stream below (GX_INDEX8/16 -> GX_DIRECT).
void ApplyAuroraVtxStateForBegin(GXVtxFmt fmt) {
    GxStream::PublishAuroraVtxState(fmt,
                                    GxStream::AuroraVtxPublishOptions{
                                        /*includeNbt=*/false,
                                        /*fmtLoopFirst=*/static_cast<int>(GX_VA_POS),
                                        /*fmtLoopLast=*/static_cast<int>(GX_VA_TEX7)});
}
}

// ============================================================================
// gxdedup: GX setter no-change early-out (Switch Diagnostics "gxdedup")
// ============================================================================
//
// The GX state setters' dirty-flag deferral follows libdol-nx by nx-mod
// (https://github.com/nx-mod/libdol-nx, GPL-3.0-or-later), whose GX behaviour
// follows doldecomp/ogws (CC0): a setter whose value already matches the live
// state does not re-emit it.
//
// While the switch is on, GX__SetVtxDesc and GX__SetVtxAttrFmt below skip their
// Aurora forward when the value they would send already equals what Aurora's
// vertex state is known to hold, and GX__SetArray skips rewriting an unchanged
// array entry (that setter has no Aurora forward; g_hleGxState is its state).
// The HLE state update above each forward — and the vertex-layout-hash
// invalidation a real change drives, which is where the pipeline keys read
// from — runs exactly as it always has. Only the Aurora call is elided, and
// only under all of these conditions:
//
// - A mirror (below) records the last value each Aurora vertex slot is known
//   to hold. The forwards maintain it, GX__ClearVtxDesc's forward sets it to
//   Aurora's post-clear registers (POS = GX_DIRECT, the rest GX_NONE), and
//   GxStream::PublishAuroraVtxState re-syncs it wholesale, since a publish
//   writes the descriptor set and one format row from g_hleGxState.
// - A played display list's own state ops rewrite the CP registers behind the
//   HLE cache's back, so GX__CallDisplayList, GXBeginDisplayList,
//   GXEndDisplayList and GXInit invalidate the mirror: after any of them a
//   slot forwards once before it can be skipped again. GX__DrawSphere
//   invalidates too — it rewrites g_hleGxState and calls Aurora's clear
//   directly, outside these overrides.
// - While a display list is being RECORDED into guest memory the call is the
//   list's content and must always emit, so the skip is gated on the tree's
//   own recording predicate, GxWorkerRecordingBlocked() (gx_init.cpp):
//   GXBeginDisplayList capture (g_dlRecordState) or an Aurora-recorded list
//   (g_spmAuroraDlActive) in progress means never skip.
// - GX_VA_NBT calls always forward: the pseudo-attribute also drives Aurora's
//   hasBiNrms/nrmType side state, which the value mirror does not model.
//
// The mirror and counters are touched wherever the setter bodies run — the
// game thread, or the GX worker's replay in record order — exactly like
// g_hleGxState itself, and need no more synchronisation than it has. With the
// switch off, each setter pays one atomic load and behaves exactly as base.

namespace GxDedup {
namespace {
std::atomic<bool> g_enabled{false};

GXAttrType g_mirrorDesc[26]{};
bool g_mirrorDescValid[26]{};
VtxAttrFmt g_mirrorFmt[8][26]{};
bool g_mirrorFmtValid[8][26]{};

struct FamilyCounter {
    std::atomic<uint64_t> total{0};
    std::atomic<uint64_t> skipped{0};
};
FamilyCounter g_counters[3];

GXAttrType MapDescType(uint32_t t) {
    return (t == GX_INDEX8 || t == GX_INDEX16) ? GX_DIRECT : static_cast<GXAttrType>(t);
}
}  // namespace

bool Enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void CountCall(int family) noexcept {
    g_counters[family].total.fetch_add(1, std::memory_order_relaxed);
}
void CountSkip(int family) noexcept {
    g_counters[family].skipped.fetch_add(1, std::memory_order_relaxed);
}
void TakeCounts(uint64_t totals[3], uint64_t skipped[3]) {
    for (int i = 0; i < 3; ++i) {
        totals[i] = g_counters[i].total.exchange(0, std::memory_order_relaxed);
        skipped[i] = g_counters[i].skipped.exchange(0, std::memory_order_relaxed);
    }
}

void NoteDescForwarded(uint32_t attr, GXAttrType mapped) noexcept {
    if (attr < 26) {
        g_mirrorDesc[attr] = mapped;
        g_mirrorDescValid[attr] = true;
    }
}
bool DescMatches(uint32_t attr, GXAttrType mapped) noexcept {
    return attr < 26 && g_mirrorDescValid[attr] && g_mirrorDesc[attr] == mapped;
}
void NoteFmtForwarded(uint32_t fmt, uint32_t attr, const VtxAttrFmt& value) noexcept {
    if (fmt < 8 && attr < 26) {
        g_mirrorFmt[fmt][attr] = value;
        g_mirrorFmtValid[fmt][attr] = true;
    }
}
bool FmtMatches(uint32_t fmt, uint32_t attr, const VtxAttrFmt& value) noexcept {
    if (fmt >= 8 || attr >= 26 || !g_mirrorFmtValid[fmt][attr]) return false;
    const VtxAttrFmt& held = g_mirrorFmt[fmt][attr];
    return held.cnt == value.cnt && held.type == value.type && held.frac == value.frac;
}
void NoteClearForwarded() noexcept {
    for (uint32_t attr = 0; attr < 26; ++attr) {
        g_mirrorDesc[attr] = attr == GX_VA_POS ? GX_DIRECT : GX_NONE;
        g_mirrorDescValid[attr] = attr != GX_VA_NBT;
    }
}
void NoteVtxPublish(int fmt, int fmtLoopFirst, int fmtLoopLast) {
    for (uint32_t attr = 0; attr < 26; ++attr) {
        if (attr == GX_VA_NBT) continue;
        g_mirrorDesc[attr] = MapDescType(static_cast<uint32_t>(g_hleGxState.vtxDesc[attr]));
        g_mirrorDescValid[attr] = true;
    }
    if (fmt < 0 || fmt >= 8) return;
    for (int attr = fmtLoopFirst; attr <= fmtLoopLast; ++attr) {
        if (attr < GX_VA_POS || attr >= GX_VA_MAX_ATTR) continue;
        g_mirrorFmt[fmt][attr] = g_hleGxState.vtxAttrFmt[fmt][attr];
        g_mirrorFmtValid[fmt][attr] = true;
    }
}
void InvalidateVtxMirror() noexcept {
    for (bool& valid : g_mirrorDescValid) valid = false;
    for (auto& row : g_mirrorFmtValid)
        for (bool& valid : row) valid = false;
}
}  // namespace GxDedup

// The deko diag channel's door (switch/deko/device.cpp), the same shape as
// spm_gxskip_set: diag_init and diag_set forward the "gxdedup" switch here.
extern "C" void spm_gxdedup_set(bool enabled) {
    GxDedup::g_enabled.store(enabled, std::memory_order_relaxed);
}

namespace GxWorker {
void PrintGxDedupWindow(uint64_t firstFrame, uint64_t lastFrame) {
    if (lastFrame < firstFrame) return;
    uint64_t totals[3], skipped[3];
    GxDedup::TakeCounts(totals, skipped);
    if (totals[0] == 0 && totals[1] == 0 && totals[2] == 0) {
        return;  // the switch was off (or no setter ran) for this whole window
    }
    std::fprintf(stderr,
                 "[gxdedup] frames %llu-%llu: skipped/total VtxDesc %llu/%llu, VtxAttrFmt %llu/%llu, "
                 "SetArray %llu/%llu\n",
                 static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame),
                 static_cast<unsigned long long>(skipped[GxDedup::kFamilyVtxDesc]),
                 static_cast<unsigned long long>(totals[GxDedup::kFamilyVtxDesc]),
                 static_cast<unsigned long long>(skipped[GxDedup::kFamilyVtxAttrFmt]),
                 static_cast<unsigned long long>(totals[GxDedup::kFamilyVtxAttrFmt]),
                 static_cast<unsigned long long>(skipped[GxDedup::kFamilySetArray]),
                 static_cast<unsigned long long>(totals[GxDedup::kFamilySetArray]));
}
}  // namespace GxWorker

// ============================================================================
// Vertex Descriptor
// ============================================================================

extern "C" void GX__ClearVtxDesc_8016dc34() {
    bool changed = false;
    for(int i=0; i<26; ++i){
        changed |= g_hleGxState.vtxDesc[i] != GX_NONE;
        g_hleGxState.NoteVtxDescChanged(i, g_hleGxState.vtxDesc[i], GX_NONE);
        g_hleGxState.vtxDesc[i]=GX_NONE;
    }
    if (changed) g_hleGxState.InvalidateVtxLayoutHash();
    // GXClearVtxDesc resets descriptors only; array base/stride state persists.
    GXClearVtxDesc();
    GxDedup::NoteClearForwarded();
}
PPC_NATIVE_OVERRIDE_VOID(8028bec8, GX__ClearVtxDesc_8016dc34, (), ());

extern "C" void GX__SetVtxDesc_8016d3a4(uint32_t a, uint32_t t) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if(attr>=26||attr==GX_VA_NULL) return;
    const GXAttrType oldType = g_hleGxState.vtxDesc[attr];
    const GXAttrType oldNbt = g_hleGxState.vtxDesc[GX_VA_NBT];
    g_hleGxState.vtxDesc[attr]=(GXAttrType)t;
    if (a == GX_VA_NBT) {
        g_hleGxState.vtxDesc[GX_VA_NBT] = GX_NONE;
    }
    g_hleGxState.NoteVtxDescChanged(attr, oldType, g_hleGxState.vtxDesc[attr]);
    if (attr != GX_VA_NBT) {
        g_hleGxState.NoteVtxDescChanged(GX_VA_NBT, oldNbt, g_hleGxState.vtxDesc[GX_VA_NBT]);
    }
    if (g_hleGxState.vtxDesc[attr] != oldType) g_hleGxState.InvalidateVtxLayoutHash();
    if(IsMatrixIndexAttr((GXAttr)attr)) return;
    const GXAttrType mapped = (t==GX_INDEX8||t==GX_INDEX16)?GX_DIRECT:(GXAttrType)t;
    if (GxDedup::Enabled()) {
        GxDedup::CountCall(GxDedup::kFamilyVtxDesc);
        if (a != GX_VA_NBT && GxDedup::DescMatches(attr, mapped) && !GxWorkerRecordingBlocked()) {
            GxDedup::CountSkip(GxDedup::kFamilyVtxDesc);
            return;
        }
    }
    GXSetVtxDesc((GXAttr)a, mapped);
    if (a != GX_VA_NBT) GxDedup::NoteDescForwarded(attr, mapped);
}
PPC_NATIVE_OVERRIDE_VOID(8028b638, GX__SetVtxDesc_8016d3a4, (uint32_t a, uint32_t t), (a, t));

extern "C" void GX__SetVtxDescv_8016d608(uint32_t la) {
    if(!la) return; uint32_t p=la;
    while(true){
        uint32_t a=Memory::Read32(p);
        if(a==0xFFu) break;
        GX__SetVtxDesc_8016d3a4(a, Memory::Read32(p+4));
        p+=8;
    }
}
PPC_NATIVE_OVERRIDE_VOID(8028b89c, GX__SetVtxDescv_8016d608, (uint32_t la), (la));

extern "C" void GX__GetVtxDesc_8016d9f0(uint32_t a, uint32_t tp) {
    GXAttrType type = GX_NONE;
    const uint32_t attr = CanonicalVtxAttr(a);
    if (attr < GX_VA_MAX_ATTR && attr != GX_VA_NULL) {
        type = g_hleGxState.vtxDesc[attr];
    }
    if (tp) {
        Memory::Write32(tp, static_cast<uint32_t>(type));
    }
}
PPC_NATIVE_OVERRIDE_VOID(8028bc84, GX__GetVtxDesc_8016d9f0, (uint32_t a, uint32_t tp), (a, tp));

extern "C" void GX__GetVtxDescv_8016dba4(uint32_t la) {
    if (!la) {
        return;
    }

    uint32_t p = la;
    for (uint32_t a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; ++a) {
        Memory::Write32(p, a);
        Memory::Write32(p + 4, static_cast<uint32_t>(g_hleGxState.vtxDesc[a]));
        p += 8;
    }
    Memory::Write32(p, GX_VA_NBT);
    Memory::Write32(p + 4, static_cast<uint32_t>(g_hleGxState.vtxDesc[GX_VA_NRM]));
    p += 8;
    Memory::Write32(p, GX_VA_NULL);
}
PPC_NATIVE_OVERRIDE_VOID(8028be38, GX__GetVtxDescv_8016dba4, (uint32_t la), (la));

// ============================================================================
// Vertex Attribute Format
// ============================================================================

extern "C" void GX__SetVtxAttrFmt_8016dc68(uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if(vf<8&&attr<26){
        const VtxAttrFmt oldFmt = g_hleGxState.vtxAttrFmt[vf][attr];
        const VtxAttrFmt oldNbtFmt = g_hleGxState.vtxAttrFmt[vf][GX_VA_NBT];
        g_hleGxState.vtxAttrFmt[vf][attr].cnt=(GXCompCnt)c;
        g_hleGxState.vtxAttrFmt[vf][attr].type=(GXCompType)t;
        g_hleGxState.vtxAttrFmt[vf][attr].frac=(u8)fr;
        if (a == GX_VA_NBT) {
            g_hleGxState.vtxAttrFmt[vf][GX_VA_NBT] = {};
        }
        const auto& newFmt = g_hleGxState.vtxAttrFmt[vf][attr];
        g_hleGxState.NoteVtxAttrFmtChanged(vf, attr, oldFmt, newFmt);
        if (attr != GX_VA_NBT) {
            g_hleGxState.NoteVtxAttrFmtChanged(vf, GX_VA_NBT, oldNbtFmt,
                                               g_hleGxState.vtxAttrFmt[vf][GX_VA_NBT]);
        }
        if (oldFmt.cnt != newFmt.cnt || oldFmt.type != newFmt.type || oldFmt.frac != newFmt.frac) {
            g_hleGxState.InvalidateVtxLayoutHash();
        }
    }
    if (vf >= GX_MAX_VTXFMT || a < GX_VA_POS || a >= GX_VA_MAX_ATTR) {
        return;
    }
    const VtxAttrFmt value{(GXCompCnt)c, (GXCompType)t, (u8)fr};
    if (a != GX_VA_NBT && GxDedup::Enabled()) {
        GxDedup::CountCall(GxDedup::kFamilyVtxAttrFmt);
        if (GxDedup::FmtMatches(vf, attr, value) && !GxWorkerRecordingBlocked()) {
            GxDedup::CountSkip(GxDedup::kFamilyVtxAttrFmt);
            return;
        }
    }
    GXSetVtxAttrFmt((GXVtxFmt)vf, (GXAttr)a, (GXCompCnt)c, (GXCompType)t, (u8)fr);
    if (a != GX_VA_NBT) GxDedup::NoteFmtForwarded(vf, attr, value);
}
PPC_NATIVE_OVERRIDE_VOID(8028befc, GX__SetVtxAttrFmt_8016dc68, (uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr), (vf, a, c, t, fr));

extern "C" void GX__SetVtxAttrFmtv_8016de08(uint32_t vf, uint32_t la) {
    if(!la) return; uint32_t p=la;
    while(true){
        uint32_t a=Memory::Read32(p);
        if(a==0xFFu) break;
        GX__SetVtxAttrFmt_8016dc68(vf, a, Memory::Read32(p+4), Memory::Read32(p+8), Memory::Read8(p+12));
        p+=16;
    }
}
PPC_NATIVE_OVERRIDE_VOID(8028c09c, GX__SetVtxAttrFmtv_8016de08, (uint32_t vf, uint32_t la), (vf, la));

extern "C" void GX__GetVtxAttrFmt_8016e04c(uint32_t vf, uint32_t a, uint32_t cp, uint32_t tp, uint32_t fp) {
    VtxAttrFmt fmt{};
    const uint32_t attr = CanonicalVtxAttr(a);
    if (vf < 8 && attr < GX_VA_MAX_ATTR && attr != GX_VA_NULL) {
        fmt = g_hleGxState.vtxAttrFmt[vf][attr];
    }
    if (cp) {
        Memory::Write32(cp, static_cast<uint32_t>(fmt.cnt));
    }
    if (tp) {
        Memory::Write32(tp, static_cast<uint32_t>(fmt.type));
    }
    if (fp) {
        Memory::Write8(fp, fmt.frac);
    }
}
PPC_NATIVE_OVERRIDE_VOID(8028c2e0, GX__GetVtxAttrFmt_8016e04c, (uint32_t vf, uint32_t a, uint32_t cp, uint32_t tp, uint32_t fp), (vf, a, cp, tp, fp));

extern "C" void GX__GetVtxAttrFmtv_8016e2b8(uint32_t vf, uint32_t la) {
    if (!la) {
        return;
    }

    uint32_t p = la;
    for (uint32_t a = GX_VA_POS; a <= GX_VA_TEX7; ++a) {
        VtxAttrFmt fmt{};
        if (vf < 8) {
            fmt = g_hleGxState.vtxAttrFmt[vf][a];
        }
        Memory::Write32(p, a);
        Memory::Write32(p + 4, static_cast<uint32_t>(fmt.cnt));
        Memory::Write32(p + 8, static_cast<uint32_t>(fmt.type));
        Memory::Write8(p + 12, fmt.frac);
        p += 16;
    }
    Memory::Write32(p, GX_VA_NULL);
}
PPC_NATIVE_OVERRIDE_VOID(8028c54c, GX__GetVtxAttrFmtv_8016e2b8, (uint32_t vf, uint32_t la), (vf, la));

// ============================================================================
// Vertex Arrays
// ============================================================================

extern "C" void GX__SetArray_8016e32c(uint32_t a, uint32_t ba, uint32_t str) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if(attr<26){
        // gxdedup: this setter's whole forward is the state write itself (the
        // array state lives in g_hleGxState; nothing is emitted), so an
        // unchanged entry is pure re-write. Counted for the share; the saving
        // here is the write, not a call.
        if (GxDedup::Enabled()) {
            GxDedup::CountCall(GxDedup::kFamilySetArray);
            if (g_hleGxState.vtxArray[attr].base == ba && g_hleGxState.vtxArray[attr].stride == str) {
                GxDedup::CountSkip(GxDedup::kFamilySetArray);
                return;
            }
        }
        g_hleGxState.vtxArray[attr].base=ba; g_hleGxState.vtxArray[attr].stride=str;
    }
}
PPC_NATIVE_OVERRIDE_VOID(8028c5c0, GX__SetArray_8016e32c, (uint32_t a, uint32_t ba, uint32_t str), (a, ba, str));

// Switch-artifact entry point for the same SDK function; forwards rather than
// repeating the body.
extern "C" void GX__SetArray_8016e1c4(uint32_t a, uint32_t b, uint32_t s) { GX__SetArray_8016e32c(a, b, s); }
#if 0  // NSMBW: MKW switch-artifact alias of GXSetArray (0x8016e1c4); NSMBW's map has no such split
PPC_NATIVE_OVERRIDE_VOID(8028c5c0, GX__SetArray_8016e1c4, (uint32_t a, uint32_t b, uint32_t s), (a, b, s));
#endif

// ============================================================================
// Texture Coordinate Generation
// ============================================================================

bool SpmAuroraDlRecording();
extern "C" void GX__SetNumTexGens_8016e5a4(uint32_t n) {
    GXSetNumTexGens((u8)n);
}
PPC_NATIVE_OVERRIDE_VOID(8028c838, GX__SetNumTexGens_8016e5a4, (uint32_t n), (n));

extern "C" void GX__SetTexCoordGen2_8016e37c(uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm) {
    if (sp >= static_cast<uint32_t>(GX_MAX_TEXGENSRC)) {
        uint32_t pc = 0;
        uint32_t lr = 0;
        if (auto* cpu = TryGetCpuContext()) {
            pc = cpu->pc;
            lr = cpu->lr;
        }
        RT_LOGF(RT_TAG_GX,
                "GXSetTexCoordGen2 invalid src=%u dst=%u type=%u mtx=%u norm=%u post=%u PC=0x%08X LR=0x%08X\n",
                sp, dc, f, m, n, pm, pc, lr);
    }
    GXSetTexCoordGen2((GXTexCoordID)dc, (GXTexGenType)f, (GXTexGenSrc)sp, m, (GXBool)n, pm);
}
PPC_NATIVE_OVERRIDE_VOID(8028c610, GX__SetTexCoordGen2_8016e37c, (uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm), (dc, f, sp, m, n, pm));

extern "C" void GX__EnableTexOffsets_8016f37c(uint32_t coord, uint32_t lineEnable, uint32_t pointEnable) {
    GXEnableTexOffsets(static_cast<GXTexCoordID>(coord),
                       lineEnable ? GX_TRUE : GX_FALSE,
                       pointEnable ? GX_TRUE : GX_FALSE);
}
PPC_NATIVE_OVERRIDE_VOID(8028d640, GX__EnableTexOffsets_8016f37c, (uint32_t coord, uint32_t lineEnable, uint32_t pointEnable), (coord, lineEnable, pointEnable));

extern "C" void GX__SetLineWidth_8016f314(uint32_t width, uint32_t texOffsets) {
    GXSetLineWidth(static_cast<u8>(width), static_cast<GXTexOffset>(texOffsets));
}
PPC_NATIVE_OVERRIDE_VOID(8028d5d8, GX__SetLineWidth_8016f314, (uint32_t width, uint32_t texOffsets), (width, texOffsets));

extern "C" void GX__SetPointSize_8016f348(uint32_t pointSize, uint32_t texOffsets) {
    GXSetPointSize(static_cast<u8>(pointSize), static_cast<GXTexOffset>(texOffsets));
}
PPC_NATIVE_OVERRIDE_VOID(8028d60c, GX__SetPointSize_8016f348, (uint32_t pointSize, uint32_t texOffsets), (pointSize, texOffsets));

// ============================================================================
// Begin/End Drawing
// ============================================================================

namespace {
thread_local bool g_inGxBeginOrFifo = false;
thread_local std::chrono::steady_clock::time_point g_nextDeferredTimingPoll{};

void ServiceDeferredTimingDuringGxWork() {
    // Wall-clock driven, so where it lands in the guest's execution depends on host speed; the
    // deterministic mode (det_clock.h) delivers these events only at their virtual time.
    // On the GX worker thread (gx_worker.h, Threaded) the game thread is not stuck behind this call:
    // it services its own timing, and VI and alarm state belong to it.
    if (g_inGxBeginOrFifo || DetClock::Enabled() || GxWorker::OnWorkerThread()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextDeferredTimingPoll) {
        return;
    }
    g_nextDeferredTimingPoll = now + std::chrono::milliseconds(1);

    g_inGxBeginOrFifo = true;
    try {
        if (Fiber::GuestFiberManager::IsInitialized()) {
            // VI callbacks are what advance EGG::AsyncDisplay on NTSC/PAL60;
            // PAL50 uses an OS alarm. Keep both clocks moving while a slow
            // host frame is still being submitted, but defer fiber switches
            // and Aurora work until the native GX call has unwound.
            VI_HLE_ProcessRetracesDeferred(8);
            OS_HLE_ProcessAlarmsDeferred(8);
        } else if (TryGetCpuContext() != nullptr) {
            // Same interrupt isolation as the deferred paths above: this GX call
            // is mid-execution of a translated function, so the retrace
            // callbacks must not publish their registers into its context.
            GuestInterruptCallbackContext interrupt;
            VI_HLE_PollRetrace(interrupt.get());
        }
    } catch (...) {
        g_inGxBeginOrFifo = false;
        throw;
    }
    g_inGxBeginOrFifo = false;
}
}

uint32_t SpmPresentedFrames();
static int32_t SpmDumpTarget() {
    static int32_t target = [] { std::ifstream in("drawdump.txt"); int32_t f = -1; in >> f; return f; }();
    return target;
}
bool SpmDumpFrame() {
    const int32_t t = SpmDumpTarget();
    if (t < 0) return false;
    const uint32_t f = SpmPresentedFrames();
    return f >= static_cast<uint32_t>(t) && f <= static_cast<uint32_t>(t) + 1;
}
// SPM bring-up: drawdump.txt beside the exe holds a frame number; that frame's GXBegin calls are logged.
static void SpmDumpBegin(uint32_t t, uint32_t vf, uint32_t nv) {
    static int32_t target = [] {
        std::ifstream in("drawdump.txt"); int32_t f = -1; in >> f; return f;
    }();
    if (target < 0) return;
    const uint32_t frame = SpmPresentedFrames();
    if (frame < static_cast<uint32_t>(target) || frame > static_cast<uint32_t>(target) + 1) return;
    static int n = 0;
    if (n++ > 400) return;
    const auto& d = g_hleGxState.vtxDesc;
    RT_LOGF(RT_TAG_GX, "[drawdump] f%u begin prim 0x%02X fmt %u n %u desc pos%u nrm%u c0%u c1%u t0%u t1%u pnmi%u dl%d\n", frame, t, vf, nv,
            d[GX_VA_POS], d[GX_VA_NRM], d[GX_VA_CLR0], d[GX_VA_CLR1], d[GX_VA_TEX0], d[GX_VA_TEX1], d[GX_VA_PNMTXIDX],
            SpmAuroraDlRecording() ? 1 : 0);
}

extern "C" void GX__Begin_8016f0f0(uint32_t t, uint32_t vf, uint32_t nv) {
    SpmDumpBegin(t, vf, nv);
    SpmRecordRawVertexDisplayList();
    if(IsDisplayListActive()){
        WriteDisplayListData((u8)(t|vf), 1);
        WriteDisplayListData((u16)nv, 2);
        return;
    }
    ServiceDeferredTimingDuringGxWork();
    ApplyAuroraVtxStateForBegin(static_cast<GXVtxFmt>(vf));
    g_hleGxState.currentVtxFmt=(GXVtxFmt)vf; g_hleGxState.currentPrim=(GXPrimitive)t;
    g_hleGxState.vertsRemaining=nv; g_hleGxState.inBegin=true; g_hleGxState.auroraBeginCalled=false;
    g_hleGxState.fifoReadOffset = 0;
    g_hleGxState.fifoByteCount=0; g_hleGxState.ResetVertex();
}
PPC_NATIVE_OVERRIDE_VOID(8028d3b4, GX__Begin_8016f0f0, (uint32_t t, uint32_t vf, uint32_t nv), (t, vf, nv));

extern "C" void GX__End_80044b30() { g_hleGxState.inBegin=false; GXEnd(); }
#if 0  // SPM: GXEnd: name not in SPM
PPC_NATIVE_OVERRIDE_VOID(802a35b0, GX__End_80044b30, (), ());  // NSMBW out-of-line GXEnd copy #1 (blr)
#endif

extern "C" void GX__End_80048c30() { GX__End_80044b30(); }
#if 0  // SPM: GXEnd: name not in SPM
PPC_NATIVE_OVERRIDE_VOID(802a7c90, GX__End_80048c30, (), ());  // NSMBW out-of-line GXEnd copy #2 (blr)
#endif

extern "C" void GX__DrawSphere_80172a30(uint32_t numMajor, uint32_t numMinor) {
    // gxdedup: this function rewrites g_hleGxState directly and calls Aurora's
    // GXClearVtxDesc itself, outside the overrides the mirror watches.
    GxDedup::InvalidateVtxMirror();
    constexpr uint32_t kAttrCount = 26;
    constexpr float kSphereRadius = 1.0f;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = kPi * 2.0f;

    std::array<GXAttrType, kAttrCount> savedVtxDesc{};
    std::array<VtxAttrFmt, kAttrCount> savedFmt3{};
    for (uint32_t i = 0; i < kAttrCount; ++i) {
        savedVtxDesc[i] = g_hleGxState.vtxDesc[i];
        savedFmt3[i] = g_hleGxState.vtxAttrFmt[GX_VTXFMT3][i];
    }

    const bool hadTex0 = savedVtxDesc[GX_VA_TEX0] != GX_NONE;

    bool drawOpen = false;
    try {
        EnsureAuroraFrameActive();

        for (uint32_t i = 0; i < kAttrCount; ++i) {
            g_hleGxState.NoteVtxDescChanged(i, g_hleGxState.vtxDesc[i], GX_NONE);
            g_hleGxState.vtxDesc[i] = GX_NONE;
        }
        g_hleGxState.InvalidateVtxLayoutHash();
        GXClearVtxDesc();

        GX__SetVtxDesc_8016d3a4(GX_VA_POS, GX_DIRECT);
        GX__SetVtxDesc_8016d3a4(GX_VA_NRM, GX_DIRECT);
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT3, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
        if (hadTex0) {
            GX__SetVtxDesc_8016d3a4(GX_VA_TEX0, GX_DIRECT);
            GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT3, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
        }

        const float majorStep = kPi / static_cast<float>(numMajor);
        const float minorStep = kTwoPi / static_cast<float>(numMinor);
        const float invRadius = 1.0f / kSphereRadius;

        for (uint32_t major = 0; major < numMajor; ++major) {
            const float major0 = static_cast<float>(major) * majorStep;
            const float major1 = major0 + majorStep;

            const float ring0 = kSphereRadius * std::sin(major0);
            const float ring1 = kSphereRadius * std::sin(major1);
            const float z0 = kSphereRadius * std::cos(major0);
            const float z1 = kSphereRadius * std::cos(major1);

            const uint16_t vertexCount = static_cast<uint16_t>(((numMinor + 1u) * 2u) & 0xFFFEu);
            GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT3, vertexCount);
            drawOpen = true;

            const float nz0 = z0 * invRadius;
            const float nz1 = z1 * invRadius;

            for (uint32_t minor = 0; minor <= numMinor; ++minor) {
                const float minorAngle = static_cast<float>(minor) * minorStep;
                const float c = std::cos(minorAngle);
                const float s = std::sin(minorAngle);

                const float x1 = c * ring1;
                const float y1 = s * ring1;
                GXPosition3f32(x1, y1, z1);
                GXNormal3f32(x1 * invRadius, y1 * invRadius, nz1);
                if (hadTex0) {
                    GXTexCoord2f32(static_cast<float>(minor) / static_cast<float>(numMinor),
                                   static_cast<float>(major + 1u) / static_cast<float>(numMajor));
                }

                const float x0 = c * ring0;
                const float y0 = s * ring0;
                GXPosition3f32(x0, y0, z0);
                GXNormal3f32(x0 * invRadius, y0 * invRadius, nz0);
                if (hadTex0) {
                    GXTexCoord2f32(static_cast<float>(minor) / static_cast<float>(numMinor),
                                   static_cast<float>(major) / static_cast<float>(numMajor));
                }
            }
            GXEnd();
            drawOpen = false;
        }
    } catch (...) {
        if (drawOpen) {
            GXEnd();
            drawOpen = false;
        }
    }

    for (uint32_t i = 0; i < kAttrCount; ++i) {
        g_hleGxState.NoteVtxDescChanged(i, g_hleGxState.vtxDesc[i], GX_NONE);
        g_hleGxState.vtxDesc[i] = GX_NONE;
    }
    g_hleGxState.InvalidateVtxLayoutHash();
    GXClearVtxDesc();
    for (uint32_t attr = 0; attr < kAttrCount; ++attr) {
        const GXAttrType type = savedVtxDesc[attr];
        if (type != GX_NONE) {
            GX__SetVtxDesc_8016d3a4(attr, static_cast<uint32_t>(type));
        }
    }
    for (uint32_t attr = GX_VA_POS; attr <= GX_VA_TEX7; ++attr) {
        const VtxAttrFmt& fmt = savedFmt3[attr];
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT3, attr, static_cast<uint32_t>(fmt.cnt), static_cast<uint32_t>(fmt.type), fmt.frac);
    }
}
PPC_NATIVE_OVERRIDE_VOID(80290adc, GX__DrawSphere_80172a30, (uint32_t numMajor, uint32_t numMinor), (numMajor, numMinor));
