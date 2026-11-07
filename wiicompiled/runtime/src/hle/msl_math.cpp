// MSL's __ieee754_sqrt (fdlibm e_sqrt.c, 0x80261BBC) as a native.
//
// The guest computes square roots bit by bit in integer code, and translated that loop is about
// 4% of the game thread at stock (the Nov 7 round 7 profile: the top translated function after
// the reverb). fdlibm's result is the correctly rounded square root, which is exactly what the
// host's sqrt instruction returns for any positive normal input, so that is the fast path. Every
// other input (zeros, negatives, infinities, NaNs, subnormals) goes through FdlibmSqrt below, a
// line-for-line port of e_sqrt.c: subnormals in particular, since the host may flush them to zero
// while the guest's integer loop does not.
//
// "sqrtverify" (dk_diag / Diagnostics, default off) runs the port beside the fast path on every
// call and compares the bits; the first mismatch is logged with its input and the fast path then
// stays off for the rest of the run. The host test (tests/msl_math_tests.cpp) checks both paths
// against the C library over many millions of inputs.

#if !defined(SPM_MSL_MATH_HOST_TEST)
#include "hle_stubs.h"
#endif

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace MslMath {

inline uint64_t Bits(double x) {
    uint64_t b;
    std::memcpy(&b, &x, sizeof b);
    return b;
}

inline double FromBits(uint64_t b) {
    double x;
    std::memcpy(&x, &b, sizeof x);
    return x;
}

// e_sqrt.c, kept in its own shape so it reads against the original. The one change: the
// subnormal normalisation's ix1 >> (32 - i) is defined here for i == 0 as the PowerPC's srw
// gives it (0), where C leaves a shift by 32 undefined.
double FdlibmSqrt(double x) {
    static const double one = 1.0, tiny = 1.0e-300;
    const int32_t sign = static_cast<int32_t>(0x80000000u);
    const uint64_t b = Bits(x);
    int32_t ix0 = static_cast<int32_t>(b >> 32);
    uint32_t ix1 = static_cast<uint32_t>(b);

    if ((ix0 & 0x7ff00000) == 0x7ff00000) {
        return x * x + x;  // sqrt(NaN) = NaN, sqrt(+inf) = +inf, sqrt(-inf) = NaN
    }
    if (ix0 <= 0) {
        if (((ix0 & ~sign) | static_cast<int32_t>(ix1)) == 0) {
            return x;  // sqrt(+-0) = +-0
        }
        if (ix0 < 0) {
            return (x - x) / (x - x);  // sqrt(-ve) = NaN
        }
    }
    int32_t m = ix0 >> 20;
    if (m == 0) {  // subnormal x
        while (ix0 == 0) {
            m -= 21;
            ix0 |= static_cast<int32_t>(ix1 >> 11);
            ix1 <<= 21;
        }
        int32_t i = 0;
        for (; (ix0 & 0x00100000) == 0; ++i) {
            ix0 <<= 1;
        }
        m -= i - 1;
        ix0 |= static_cast<int32_t>(i == 0 ? 0u : (ix1 >> (32 - i)));
        ix1 <<= i;
    }
    m -= 1023;
    ix0 = (ix0 & 0x000fffff) | 0x00100000;
    if (m & 1) {
        ix0 += ix0 + static_cast<int32_t>((ix1 & static_cast<uint32_t>(sign)) >> 31);
        ix1 += ix1;
    }
    m >>= 1;

    ix0 += ix0 + static_cast<int32_t>((ix1 & static_cast<uint32_t>(sign)) >> 31);
    ix1 += ix1;
    int32_t q = 0, s0 = 0;
    uint32_t q1 = 0, s1 = 0;
    uint32_t r = 0x00200000;
    while (r != 0) {
        const int32_t t = s0 + static_cast<int32_t>(r);
        if (t <= ix0) {
            s0 = t + static_cast<int32_t>(r);
            ix0 -= t;
            q += static_cast<int32_t>(r);
        }
        ix0 += ix0 + static_cast<int32_t>((ix1 & static_cast<uint32_t>(sign)) >> 31);
        ix1 += ix1;
        r >>= 1;
    }
    r = static_cast<uint32_t>(sign);
    while (r != 0) {
        const uint32_t t1 = s1 + r;
        const int32_t t = s0;
        if ((t < ix0) || ((t == ix0) && (t1 <= ix1))) {
            s1 = t1 + r;
            if (((t1 & static_cast<uint32_t>(sign)) == static_cast<uint32_t>(sign)) &&
                (s1 & static_cast<uint32_t>(sign)) == 0) {
                s0 += 1;
            }
            ix0 -= t;
            if (ix1 < t1) {
                ix0 -= 1;
            }
            ix1 -= t1;
            q1 += r;
        }
        ix0 += ix0 + static_cast<int32_t>((ix1 & static_cast<uint32_t>(sign)) >> 31);
        ix1 += ix1;
        r >>= 1;
    }

    // Floating adds find the rounding direction, as in the original.
    if ((ix0 | static_cast<int32_t>(ix1)) != 0) {
        volatile double z = one - tiny;
        if (z >= one) {
            z = one + tiny;
            if (q1 == 0xffffffffu) {
                q1 = 0;
                q += 1;
            } else if (z > one) {
                if (q1 == 0xfffffffeu) {
                    q += 1;
                }
                q1 += 2;
            } else {
                q1 += (q1 & 1);
            }
        }
    }
    int32_t hi = (q >> 1) + 0x3fe00000;
    uint32_t lo = q1 >> 1;
    if ((q & 1) == 1) {
        lo |= static_cast<uint32_t>(sign);
    }
    hi += static_cast<int32_t>(static_cast<uint32_t>(m) << 20);
    return FromBits((static_cast<uint64_t>(static_cast<uint32_t>(hi)) << 32) | lo);
}

// True for a positive normal double: sign clear, exponent neither 0 nor all ones.
inline bool PositiveNormal(uint64_t b) {
    const uint32_t exponent = static_cast<uint32_t>(b >> 52) & 0x7ffu;
    return (b >> 63) == 0 && exponent != 0 && exponent != 0x7ffu;
}

std::atomic<bool> g_verify{false};
std::atomic<bool> g_fastDisabled{false};

double Sqrt(double x) {
    const uint64_t b = Bits(x);
    if (!PositiveNormal(b) || g_fastDisabled.load(std::memory_order_relaxed)) {
        return FdlibmSqrt(x);
    }
    const double fast = std::sqrt(x);
    if (g_verify.load(std::memory_order_relaxed)) {
        const double reference = FdlibmSqrt(x);
        if (Bits(reference) != Bits(fast)) {
            if (!g_fastDisabled.exchange(true, std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "[sqrtverify] MISMATCH: input %016llx, fast %016llx, fdlibm %016llx; the fast path is off "
                             "for the rest of this run\n",
                             static_cast<unsigned long long>(b), static_cast<unsigned long long>(Bits(fast)),
                             static_cast<unsigned long long>(Bits(reference)));
            }
            return reference;
        }
    }
    return fast;
}

} // namespace MslMath

// The sqrtverify switch's hook from the deko diag layer (device.cpp), the same pattern as the
// GX worker switches: diag_init hands it the dk_diag.txt value, diag_set forwards every flip.
extern "C" void spm_sqrtverify_set(bool enabled) {
    MslMath::g_verify.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        std::fprintf(stderr, "[sqrtverify] on: every __ieee754_sqrt call checks the fast path against fdlibm\n");
    }
}

#if !defined(SPM_MSL_MATH_HOST_TEST)
extern "C" double MSL__ieee754_sqrt_HLE_80261bbc(double x) { return MslMath::Sqrt(x); }

PPC_NATIVE_OVERRIDE(80261BBC, MSL__ieee754_sqrt_HLE_80261bbc, double, (double x), (x));
#endif
