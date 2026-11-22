#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <unistd.h>
static int slowed;
static int slow(int fd)
{
    char relative[32], path[4096];
    snprintf(relative, sizeof relative, "/proc/self/fd/%d", fd);
    ssize_t count = readlink(relative, path, sizeof(path) - 1);
    if (count <= 0) return 0;
    path[count] = 0;
    if (!strstr(path, ".files-transfer-")) return 0;
    const char *after = getenv("FILES_COPY_SLOW_AFTER");
    if (after && lseek(fd, 0, SEEK_CUR) < atoll(after)) return 0;
    if (__atomic_exchange_n(&slowed, 1, __ATOMIC_SEQ_CST)) return 0;
    const char *marker_path = getenv("FILES_COPY_MARKER");
    FILE *marker = marker_path ? fopen(marker_path, "w") : NULL;
    if (marker) fclose(marker);
    sleep(3);
    if (!getenv("FILES_AUDIT_KEEP_STAGE")) return 0;
    errno = EIO;
    return 1;
}
ssize_t sendfile(int out, int in, off_t *offset, size_t count)
{
    ssize_t (*next)(int, int, off_t *, size_t) = dlsym(RTLD_NEXT, "sendfile");
    if (slow(out)) return -1;
    return next(out, in, offset, count);
}
ssize_t write(int fd, const void *data, size_t count)
{
    ssize_t (*next)(int, const void *, size_t) = dlsym(RTLD_NEXT, "write");
    if (count >= 65536 && slow(fd)) return -1;
    return next(fd, data, count);
}

// Simulate a device rejecting cleanup of an interrupted copy's private payload.
int unlinkat(int dirfd, const char *name, int flags)
{
    int (*next)(int, const char *, int) = dlsym(RTLD_NEXT, "unlinkat");
    if (getenv("FILES_AUDIT_KEEP_STAGE") && strcmp(name, "payload") == 0) {
        char relative[64], target[4096];
        snprintf(relative, sizeof relative, "/proc/self/fd/%d", dirfd);
        const ssize_t count = readlink(relative, target, sizeof(target) - 1);
        if (count > 0) {
            target[count] = 0;
            if (strstr(target, "/.files-transfer-")) {
                errno = EIO;
                return -1;
            }
        }
    }
    return next(dirfd, name, flags);
}
