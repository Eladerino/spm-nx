// POSIX calls newlib declares but libnx does not provide.
#include <errno.h>
#include <unistd.h>

// Horizon has no pipes. The runtime's process transcript (main.cpp) pipes stdout and stderr through
// a thread into console.log; when pipe() fails it puts the streams back and carries on, so stderr
// keeps going to UserData/Logs/stderr.log (source/spm_switch_init.cpp).
int pipe(int fds[2])
{
    (void)fds;
    errno = ENOSYS;
    return -1;
}
