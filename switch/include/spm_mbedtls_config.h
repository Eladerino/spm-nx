// mbed TLS user config for the Switch build (MBEDTLS_USER_CONFIG_FILE, set by switch/cmake/SpmSwitch.cmake;
// appended to mbed TLS's default config). mbed TLS only knows Unix and Windows for its three OS-facing
// modules; Horizon gets each one from switch/runtime/horizon_mbedtls.c instead:
//   entropy  libnx's randomGet() as a "hardware" entropy source (the kernel seeds it)
//   time     the CPU's system tick for mbedtls_ms_time (TLS session timing)
//   sockets  mbedtls_net_recv over libnx's BSD sockets, the one call of net_sockets.c the runtime uses
#pragma once

#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
