#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ssize_t read(int fd, void *buffer, size_t size)
{
    const char *fixture = getenv("FILES_AUDIT_READ_FAULT");
    if (fixture) {
        char link[64], target[4096];
        snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        ssize_t length = readlink(link, target, sizeof target - 1);
        if (length >= 0) {
            target[length] = '\0';
            if (strcmp(target, fixture) == 0) {
                errno = getenv("FILES_AUDIT_DENY_READ") ? EACCES : EIO;
                return -1;
            }
        }
    }

    ssize_t (*next)(int, void *, size_t) = dlsym(RTLD_NEXT, "read");
    return next(fd, buffer, size);
}
