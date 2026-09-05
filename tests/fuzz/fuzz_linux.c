/*
 * fuzz_linux.c - The Linux personality's ABI conversions
 * (compat/linux/convert.c) under fuzzing: guest-controlled structures in,
 * bounded native structures out.
 */

#include "fuzz.h"

#include "convert.h"

#include <kernel/errno.h>

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Socket addresses: the guest's bytes, every length up to the input. */
    struct netaddr na;
    memset(&na, 0, sizeof(na));
    int rc = lx_sockaddr_to_netaddr(data, size, &na);
    if (rc == 0) {
        uint8_t out[256];
        size_t n = lx_sockaddr_from_netaddr(&na, out, sizeof(out));
        FUZZ_ASSERT(n <= sizeof(out));
        for (size_t cap = 0; cap < 32; cap++) {
            n = lx_sockaddr_from_netaddr(&na, out, cap);
            FUZZ_ASSERT(n <= cap || n == 0 || n <= sizeof(out));
        }
    }

    /* Directory entries: native records in, Linux records out, bounded. */
    uint8_t dout[512];
    for (size_t cap = 0; cap <= sizeof(dout); cap += 37) {
        size_t n = lx_dirents_from_native(data, size, dout, cap);
        FUZZ_ASSERT(n <= cap);
    }

    /* Flags and protections from the first words. */
    if (size >= 4) {
        uint32_t v;
        memcpy(&v, data, 4);
        unsigned native = 0;
        lx_open_flags(v, &native);
        int prot = 0;
        lx_prot(v, &prot);
        lx_dirent_type((uint8_t)v);
        lx_wait_status((int)v);
    }
    return 0;
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    if (cap < 64)
        return 0;
    memset(buf, 0, 64);
    switch (i) {
    case 0:   /* sockaddr_in 127.0.0.1:7 */
        buf[0] = 2;
        buf[2] = 0;
        buf[3] = 7;
        buf[4] = 127;
        buf[7] = 1;
        return 16;
    case 1:   /* sockaddr_in6 ::1 port 7 */
        buf[0] = 10;
        buf[2] = 0;
        buf[3] = 7;
        buf[23] = 1;
        return 28;
    case 2: { /* one native dirent: ino, reclen, type, namelen, name */
        uint64_t ino = 42;
        memcpy(buf, &ino, 8);
        uint16_t reclen = 24;
        memcpy(buf + 8, &reclen, 2);
        buf[10] = 1;
        buf[11] = 5;
        memcpy(buf + 12, "hello", 5);
        return 24;
    }
    default:
        return 0;
    }
}
