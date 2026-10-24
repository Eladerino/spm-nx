// The Horizon pieces mbed TLS leaves to the platform; see switch/include/spm_mbedtls_config.h.
#include <errno.h>
#include <stddef.h>
#include <sys/socket.h>

#include <mbedtls/build_info.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/ssl.h>

#include <switch.h>

// Prototype from mbed TLS's library/entropy_poll.h (not an installed header).
int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen);

int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen)
{
    (void)data;
    randomGet(output, len);
    *olen = len;
    return 0;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(armTicksToNs(armGetSystemTick()) / 1000000u);
}

// Same contract as net_sockets.c's version: bytes read, 0 at end of stream, or an mbed TLS error.
int mbedtls_net_recv(void* ctx, unsigned char* buf, size_t len)
{
    const int fd = ((mbedtls_net_context*)ctx)->fd;
    if (fd < 0) {
        return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    }
    const ssize_t ret = recv(fd, buf, len, 0);
    if (ret >= 0) {
        return (int)ret;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (errno == EPIPE || errno == ECONNRESET) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}
