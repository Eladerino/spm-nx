// Host-native replacement for the MSL __ieee754_sqrt body (guest
// 0x80261BBC, reached from the sqrt thunk at 0x80261E08).
//
// Feasibility: ~/workspace/spm-sqrt-native-feasibility-2026-11-09.md
// (verdict (b): sound only with side-channel emulation) and the gate
// ledger in ~/workspace/spm-sqrt-gates-results.md (section 81), which
// pins every helper, address and variant used below. Read both before
// touching this file.
//
// Routing (G3, corrected at desk): no runtime registration mechanism
// reaches the thunk's 164 static call sites on Switch (ModHooks is
// constexpr-off, the indirect-call override is bctrl-only, and native
// registration is bypassed by shard static calls). The build therefore
// patches the work copy of shard_8aabd1f4f54a986e867e5f4b.cpp: the
// generated body is renamed to func_80261BBC_orig and a one-line
// func_80261BBC wrapper calls mkw_sqrt_native below. Every path to the
// unit -- the 164 static sites, the thunk, and direct indirect-dispatch
// entry at either guest address -- lands in the patched body, so this
// one native serves both addresses for all traffic. The generated tree
// in the repo is never edited; the patch lives with the build.
//
// Semantics, transcribed from the generated body (func_80261BBC in
// shard_8aabd1f4f54a986e867e5f4b.cpp). The body is the fdlibm-family
// digit-by-digit integer square root: it computes the exact root
// rounded to nearest-even for positive finite inputs and never reads
// FPSCR. By input class of f1.d:
//
//  - Prologue (every path): FlatWriteRam32([SP-32] <- entry SP) and
//    FlatWriteRamFloat64([SP-24] <- input). SP here is the entry value
//    of gpr[1]; the body's exit restores gpr[1] to it, so this native
//    simply never writes gpr[1].
//  - Exponent field all ones (+Inf, -Inf, every NaN): f0 = x * x (two
//    host double ops, in this order), write 33 to the error cell, and
//    return x + f0. FSQRT's quiet-and-propagate NaN handling is a
//    different function of the input bits, so this class is replicated
//    with the same two operations, never FSQRT.
//  - Zero of either sign: return the input unchanged. No result store.
//  - Negative nonzero (finite or denormal): write 33 to the error
//    cell and return the guest float constant at 0x8056D218 widened to
//    double. The constant is loaded live through the body's own
//    FlatReadFloat32; it is never hardcoded (gate G4).
//  - Positive finite: the exact RN-even root. On the normal class a
//    host FSQRT is bit-identical by uniqueness of correct rounding.
//    A positive DENORMAL under guest FPSCR[NI] is the one exception:
//    the runtime mirrors NI into host FPCR.FZ, under which FSQRT
//    flushes a denormal input to +0 while the body's integer core is
//    NI-immune. That class routes on the LIVE ctx->fpscr & 0x4 (never
//    on a census) to the original generated body (the fallback below),
//    so it stays correct by construction. NI is set by OSInit and
//    never cleared on any standard path, so on device this is a real
//    production path, not an exotic one (section 81, G2).
//  - Normal-path frame: the result double is stored at [SP-16] with
//    FlatWriteRamFloat64, exactly where the body's integer assembly
//    leaves it. The special/zero/negative paths store nothing there.
//
// The error cell (gate G4) is written through MemoryInline::FlatWrite32
// -- the policy-checked variant the body itself uses at both of its
// write sites -- at the address computed from the live ctx->gpr[13]
// minus 29872. FlatWriteRam32 would produce the same bytes here but is
// a different behaviour under a write policy; the pin matters.
//
// Deliberate non-replication: the body's CR, XER.CA, GPR scratch
// (r0, r3-r12) and its rounding-path f0 traffic are not reproduced.
// The translator's own ABI metadata for the body marks all of them
// clobbered, and the feasibility study's caller census found no
// consumer at any of the 164 call sites. f0 IS set on the
// exponent-all-ones path because there it is one of the two replicated
// operations' results, not scratch.
//
// Build flags (gate G5): this TU is in MKW_PPC_SEMANTIC_RUNTIME_SOURCES
// (-fno-fast-math -ffp-contract=off, unity build excluded), so the
// special path's FMUL/FADD cannot be folded, reassociated or deleted
// as dead code, and FSQRT is emitted as the instruction on AArch64.
//
// Instrumentation (gate G2): a call counter and a fallback counter,
// read through mkw_sqrt_native_calls() / mkw_sqrt_native_fallbacks()
// and echoed to stderr every 262144 calls in the shape the campaign's
// A/B logs already use (the fnprobe / bboxcache window lines), so the
// console A/B can report the denormal fallback share per scene.

#include "memory_access.h"
#include "ppc_runtime.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

// The original generated body, renamed in the patched work copy (see
// the routing note above). Weak so this TU still links in an unpatched
// build of the branch: there the symbol is absent and the fallback
// path uses the FZ-bracketed FSQRT at the bottom of this file instead.
// In the patched A/B build the strong shard definition binds.
extern "C" __attribute__((weak)) void func_80261BBC_orig(CpuContext* ctx);

namespace {

inline constexpr uint32_t kErrorCellValue = 33;
inline constexpr uint32_t kErrorCellOffset = 29872; // from ctx->gpr[13]
inline constexpr uint32_t kNaNConstantAddress = 0x8056D218u;
inline constexpr uint32_t kFpscrNiBit = 0x4u;
inline constexpr uint64_t kLogWindowCalls = 262144; // 2^18; the window test is a mask

std::atomic<uint64_t> g_sqrtCalls{0};
std::atomic<uint64_t> g_sqrtFallbacks{0};

// FSQRT on AArch64 (the differential in gate G1 compares against the
// generated body, so the instruction itself -- not a libcall whose
// lowering could vary -- is what the normal path must execute). On
// other hosts std::sqrt is the same correctly rounded operation; the
// host differential harness relies on that.
inline double HostSqrt(double value) noexcept {
#if defined(__aarch64__)
    double result;
    __asm__("fsqrt %d0, %d1" : "=w"(result) : "w"(value));
    return result;
#else
    return std::sqrt(value);
#endif
}

// Positive-denormal root with the host flush-to-zero state cleared for
// the duration of the call. Only reachable in an unpatched build (weak
// func_80261BBC_orig absent); the patched build routes the class to
// the original body instead. The restore goes through
// MkwRestoreHostMxcsr so the thread-local NI mirror stays in agreement
// with the register, per that helper's contract.
inline double HostSqrtFlushCleared(double value) noexcept {
    const uint32_t saved = MkwGetHostFpControl();
    MkwApplyHostNiMode(0);
    const double result = HostSqrt(value);
    MkwRestoreHostMxcsr(saved);
    return result;
}

} // namespace

extern "C" uint64_t mkw_sqrt_native_calls() noexcept {
    return g_sqrtCalls.load(std::memory_order_relaxed);
}

extern "C" uint64_t mkw_sqrt_native_fallbacks() noexcept {
    return g_sqrtFallbacks.load(std::memory_order_relaxed);
}

extern "C" void mkw_sqrt_native(CpuContext* ctx) {
    const uint64_t callIndex = g_sqrtCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((callIndex & (kLogWindowCalls - 1)) == 0) {
        std::fprintf(stderr, "[sqrt] calls %llu fallbacks %llu\n",
                     static_cast<unsigned long long>(callIndex),
                     static_cast<unsigned long long>(
                         g_sqrtFallbacks.load(std::memory_order_relaxed)));
    }

    const double input = ctx->fpr[1].d;
    const uint64_t bits = ctx->fpr[1].raw;
    const uint32_t hi = static_cast<uint32_t>(bits >> 32);
    const uint32_t lo = static_cast<uint32_t>(bits & 0xFFFFFFFFu);
    const uint32_t entrySp = ctx->gpr[1];

    // Prologue frame stores, on every path (see the class table above).
    MemoryInline::FlatWriteRam32(entrySp - 32, entrySp);
    MemoryInline::FlatWriteRamFloat64(entrySp - 24, input);

    // Exponent field all ones: +Inf, -Inf, all NaNs.
    if ((hi & 0x7FF00000u) == 0x7FF00000u) {
        const double f0 = input * input;
        MemoryInline::FlatWrite32(ctx->gpr[13] - kErrorCellOffset, kErrorCellValue);
        ctx->fpr[0].d = f0;
        ctx->fpr[1].d = input + f0;
        return;
    }

    if (static_cast<int32_t>(hi) <= 0) {
        const uint32_t magnitude = (hi & 0x7FFFFFFFu) | lo;
        if (magnitude == 0) {
            return; // Zero of either sign: input returned unchanged.
        }
        if (static_cast<int32_t>(hi) < 0) {
            // Negative nonzero: error cell + the live guest constant.
            MemoryInline::FlatWrite32(ctx->gpr[13] - kErrorCellOffset, kErrorCellValue);
            ctx->fpr[1].d =
                static_cast<double>(MemoryInline::FlatReadFloat32(kNaNConstantAddress));
            return;
        }
        // hi == 0 with nonzero magnitude: a positive denormal whose
        // significand sits entirely in the low word. Falls through to
        // the positive path below, exactly as the body does.
    }

    // Positive finite nonzero. A denormal (exponent field zero) under
    // guest NI routes to the original body: host FZ would flush the
    // input before FSQRT could see it.
    const bool isDenormal = (hi & 0x7FF00000u) == 0;
    if (isDenormal && (ctx->fpscr & kFpscrNiBit) != 0) {
        g_sqrtFallbacks.fetch_add(1, std::memory_order_relaxed);
        if (func_80261BBC_orig != nullptr) {
            func_80261BBC_orig(ctx);
            return;
        }
        const double flushed = HostSqrtFlushCleared(input);
        MemoryInline::FlatWriteRamFloat64(entrySp - 16, flushed);
        ctx->fpr[1].d = flushed;
        return;
    }

    const double result = HostSqrt(input);
    MemoryInline::FlatWriteRamFloat64(entrySp - 16, result);
    ctx->fpr[1].d = result;
}
