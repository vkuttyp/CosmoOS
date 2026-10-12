/*
 * aslr-native.c - roadmap M5's layout probe for the native personality
 * (docs/kernel/process/testing.md, "test-security"). A native program is
 * static and keeps its link address; its stack and its mmap base are
 * drawn at every spawn and exec like a Linux one's. The native door has no
 * break. One line:
 *
 *   aslr: kind=native stack=0x... sp=0x... mmap=0x... load=0x...
 *
 * then the native mmap door's W^X answer:
 *
 *   aslr: wx mmap-wx=EINVAL mprotect-wx=EINVAL
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

static const char *errname(int e)
{
    return e == EINVAL ? "EINVAL" : e == EACCES ? "EACCES" : e == EPERM ? "EPERM" : "other";
}

int main(int argc, char **argv)
{
    (void)argc;
    volatile int local = 0;
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("aslr: kind=native stack=%p sp=%p mmap=%p load=%p\n", (void *)argv, (void *)(uintptr_t)&local, m,
           (void *)(uintptr_t)main);
    void *wx = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    const char *mmap_result = wx == MAP_FAILED ? errname(errno) : "mapped";
    uintptr_t page = (uintptr_t)&local & ~(uintptr_t)4095;
    int rc = mprotect((void *)page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    printf("aslr: wx mmap-wx=%s mprotect-wx=%s\n", mmap_result, rc < 0 ? errname(errno) : "changed");
    return 0;
}
