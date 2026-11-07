// Host test for src/hle/msl_math.cpp: the fdlibm port and the fast path both equal the C
// library's correctly rounded sqrt, bit for bit, over random and edge-case inputs.
//   g++ -std=c++20 -O2 -DSPM_MSL_MATH_HOST_TEST tests/msl_math_tests.cpp -o /tmp/msl_math_tests
#define SPM_MSL_MATH_HOST_TEST 1
#include "../src/hle/msl_math.cpp"

#include <cinttypes>
#include <random>

namespace {

int g_failures = 0;

void Expect(double x, double got, double want, const char* what) {
    const bool bothNan = std::isnan(got) && std::isnan(want);
    if (!bothNan && MslMath::Bits(got) != MslMath::Bits(want)) {
        if (++g_failures <= 20) {
            std::fprintf(stderr, "FAIL %s: input %016" PRIx64 " got %016" PRIx64 " want %016" PRIx64 "\n", what,
                         MslMath::Bits(x), MslMath::Bits(got), MslMath::Bits(want));
        }
    }
}

void Check(double x) {
    const double want = std::sqrt(x);
    Expect(x, MslMath::FdlibmSqrt(x), want, "fdlibm");
    Expect(x, MslMath::Sqrt(x), want, "native");
}

} // namespace

int main() {
    const uint64_t edges[] = {
        0, 0x8000000000000000ull, 1, 2, 3, 0x000fffffffffffffull, 0x0010000000000000ull, 0x0000000100000000ull,
        0x00000000ffffffffull, 0x0008000000000000ull, 0x0000100000000000ull, 0x3ff0000000000000ull,
        0x4000000000000000ull, 0x4010000000000000ull, 0x7fefffffffffffffull, 0x7ff0000000000000ull,
        0xfff0000000000000ull, 0x7ff8000000000000ull, 0xbff0000000000000ull, 0x3fefffffffffffffull,
        0x3ff0000000000001ull, 0x8000000000000001ull,
    };
    for (uint64_t e : edges) {
        Check(MslMath::FromBits(e));
    }
    // Every subnormal pattern with a single bit, and every leading-bit position with random tails
    // (exercises each branch of the normalisation, including the i == 0 case).
    std::mt19937_64 rng(20261107);
    for (int bit = 0; bit < 52; ++bit) {
        Check(MslMath::FromBits(1ull << bit));
        for (int k = 0; k < 2000; ++k) {
            const uint64_t tail = rng() & ((1ull << bit) - 1);
            Check(MslMath::FromBits((1ull << bit) | tail));
        }
    }
    // Random positive doubles over the whole range, random bit patterns, and squares of random
    // doubles (exact roots, the rounding branch's other side).
    for (int k = 0; k < 20000000; ++k) {
        Check(MslMath::FromBits(rng() & 0x7fffffffffffffffull));
    }
    for (int k = 0; k < 2000000; ++k) {
        Check(MslMath::FromBits(rng()));
        const double r = std::ldexp(static_cast<double>(rng() >> 38), static_cast<int>(rng() % 200) - 100);
        Check(r * r);
        Check(std::nextafter(r * r, 0.0));
        Check(std::nextafter(r * r, 1e300));
    }
    // The verify path agrees and never disables itself on correct input.
    spm_sqrtverify_set(true);
    for (int k = 0; k < 200000; ++k) {
        Check(MslMath::FromBits(rng() & 0x7fffffffffffffffull));
    }
    spm_sqrtverify_set(false);
    if (MslMath::g_fastDisabled.load()) {
        std::fprintf(stderr, "FAIL: verify disabled the fast path\n");
        ++g_failures;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "msl_math_tests: %d failures\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "msl_math_tests: all passed\n");
    return 0;
}
