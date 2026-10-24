// ifaddrs.h for Horizon (Switch build only). Netplay (runtime/src/netplay/net_socket.cpp) lists the
// host's interface addresses to show them in the lobby. libnx has no getifaddrs; this one reports
// ENOSYS (switch/runtime/horizon_net.c), so the lobby simply has no local address to show. Netplay
// over libnx's BSD sockets otherwise compiles as on Linux.
#pragma once

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs {
    struct ifaddrs* ifa_next;
    char* ifa_name;
    unsigned int ifa_flags;
    struct sockaddr* ifa_addr;
    struct sockaddr* ifa_netmask;
    struct sockaddr* ifa_dstaddr;
    void* ifa_data;
};
// glibc spells the broadcast address as an alias of the point-to-point one, the same way.
#define ifa_broadaddr ifa_dstaddr

int getifaddrs(struct ifaddrs** ifap);
void freeifaddrs(struct ifaddrs* ifa);

#ifdef __cplusplus
}
#endif
