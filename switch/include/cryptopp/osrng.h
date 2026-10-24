// Wraps Crypto++'s osrng.h on the Switch build (switch/include comes before runtime/third_party on
// the include path, so <cryptopp/osrng.h> lands here first and #include_next reaches the real one).
//
// Crypto++ only defines AutoSeededRandomPool where it knows the OS entropy source (/dev/urandom on
// Unix, BCryptGenRandom on Windows); Horizon is neither, so the real header defines nothing. The
// runtime needs it in one place: the nonce for signing Wii ES data (include/wii_es_crypto.h). This
// supplies a drop-in with the same name, backed by libnx's randomGet() (a CSPRNG the kernel seeds).
#pragma once

#include_next <cryptopp/osrng.h>

#if defined(__SWITCH__) && !defined(OS_RNG_AVAILABLE)

#include <cryptopp/cryptlib.h>
#include <string>

extern "C" void randomGet(void* buf, size_t len);

NAMESPACE_BEGIN(CryptoPP)

class AutoSeededRandomPool : public RandomNumberGenerator {
public:
    explicit AutoSeededRandomPool(bool blocking = false, unsigned int seedSize = 32) {
        (void)blocking;
        (void)seedSize;
    }
    void GenerateBlock(byte* output, size_t size) override { randomGet(output, size); }
    std::string AlgorithmName() const override { return "HorizonRandom"; }
};

NAMESPACE_END

#endif
