#include <sys/mman.h>
#include <errno.h>

int mprotect(void *addr, size_t len, int prot) {
    (void)addr;
    (void)len;
    (void)prot;
    return 0;
}

int msync(void *addr, size_t length, int flags) {
    (void)addr;
    (void)length;
    (void)flags;
    return 0;
}

int madvise(void *addr, size_t length, int advice) {
    (void)addr;
    (void)length;
    (void)advice;
    return 0;
}
