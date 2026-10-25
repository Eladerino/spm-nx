// POSIX calls newlib declares but libnx does not provide, or provides only as ENOSYS stubs.
#include <errno.h>
#include <sys/stat.h>
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

// The SD card has no permission bits, and libnx's fsdev chmod and fchmod fail with ENOSYS. libstdc++'s
// std::filesystem::copy_file calls fchmod on the new file to give it the source's mode and fails when
// that does, so every copy failed: the managed NAND's first-run WC24 files (nand_path.h) could not be
// created and the game stopped in NANDInit. Changing nothing is the right answer here, so these
// report success for a file or descriptor that exists.
int fchmod(int fd, mode_t mode)
{
    (void)mode;
    struct stat st;
    return fstat(fd, &st);
}

int chmod(const char* path, mode_t mode)
{
    (void)mode;
    struct stat st;
    return stat(path, &st);
}
