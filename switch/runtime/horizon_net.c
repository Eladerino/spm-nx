// The Horizon side of switch/include/ifaddrs.h.
#include <ifaddrs.h>

#include <errno.h>
#include <stddef.h>

int getifaddrs(struct ifaddrs** ifap)
{
    if (ifap != NULL) *ifap = NULL;
    errno = ENOSYS;
    return -1;
}

void freeifaddrs(struct ifaddrs* ifa)
{
    (void)ifa;
}
