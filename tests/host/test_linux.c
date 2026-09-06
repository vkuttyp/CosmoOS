/*
 * test_linux.c - Host test of the Linux personality's pure conversions
 * (docs/compat/linux/testing.md): open flags, stat, wait status,
 * sockaddr both ways, getdents64 records, PROT bits. ASan and UBSan.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <kernel/errno.h>

#include "convert.h"

static int g_failures;
#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (!(c)) {                                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                         \
            g_failures++;                                                                 \
        }                                                                                 \
    } while (0)

int main(void)
{
    /* layouts */
    CHECK(sizeof(struct lx_stat) == LX_STAT_SIZE);
    CHECK(sizeof(struct lx_utsname) == 390);
    CHECK(sizeof(struct lx_sockaddr_in) == 16 && sizeof(struct lx_sockaddr_in6) == 28);
    CHECK(sizeof(struct lx_sigaction) == 32);

    /* open flags */
    unsigned n;
    CHECK(lx_open_flags(LX_O_RDONLY, &n) == 0 && n == COSMO_O_RDONLY);
    CHECK(lx_open_flags(LX_O_WRONLY | LX_O_CREAT | LX_O_TRUNC | LX_O_CLOEXEC, &n) == 0 &&
          n == (COSMO_O_WRONLY | COSMO_O_CREAT | COSMO_O_TRUNC));
    CHECK(lx_open_flags(LX_O_RDWR | LX_O_APPEND | LX_O_EXCL | LX_O_DIRECTORY | LX_O_NONBLOCK, &n) == 0 &&
          n == (COSMO_O_RDWR | COSMO_O_APPEND | COSMO_O_EXCL | COSMO_O_DIRECTORY));
    CHECK(lx_open_flags(3, &n) < 0);            /* bad access mode */
    CHECK(lx_open_flags(0x80000000u, &n) < 0);  /* unknown flag */

    /* stat */
    struct cosmo_stat st = { .ino = 7, .type = COSMO_DT_DIR, .mode = 0755, .nlink = 2, .uid = 1, .gid = 2, .size = 4096,
                             .mtime_ns = 1500000000ull, .ctime_ns = 2000000000ull };
    struct lx_stat ls;
    lx_stat_from_native(&st, &ls);
    CHECK(ls.st_ino == 7 && ls.st_mode == (LX_S_IFDIR | 0755) && ls.st_nlink == 2 && ls.st_uid == 1 && ls.st_gid == 2);
    CHECK(ls.st_size == 4096 && ls.st_blksize == 4096 && ls.st_blocks == 8);
    CHECK(ls.st_mtime == 1 && ls.st_mtime_nsec == 500000000 && ls.st_ctime == 2 && ls.st_ctime_nsec == 0);
    st.type = COSMO_DT_CHR;
    lx_stat_from_native(&st, &ls);
    CHECK((ls.st_mode & LX_S_IFMT) == LX_S_IFCHR);
    st.type = COSMO_DT_FIFO;
    lx_stat_from_native(&st, &ls);
    CHECK((ls.st_mode & LX_S_IFMT) == LX_S_IFIFO);
    st.type = COSMO_DT_SOCK;
    lx_stat_from_native(&st, &ls);
    CHECK((ls.st_mode & LX_S_IFMT) == LX_S_IFSOCK);
    st.type = COSMO_DT_REG;
    lx_stat_from_native(&st, &ls);
    CHECK((ls.st_mode & LX_S_IFMT) == LX_S_IFREG);

    /* wait status */
    CHECK(lx_wait_status(0) == 0 && lx_wait_status(7) == 7 << 8 && lx_wait_status(255) == 255 << 8);
    CHECK(lx_wait_status(128 + 9) == 9 && lx_wait_status(128 + 15) == 15);
    CHECK(lx_wait_status(COSMO_EXIT_FAULT) == LX_SIGSEGV);

    /* sockaddr in */
    struct lx_sockaddr_in a4 = { .sin_family = LX_AF_INET, .sin_port = htons(8080), .sin_addr = 0x0100007f };
    struct netaddr na;
    CHECK(lx_sockaddr_to_netaddr(&a4, sizeof(a4), &na) == 0 && na.family == COSMO_AF_INET && na.port == 8080 &&
          na.v4 == 0x0100007f);
    CHECK(lx_sockaddr_to_netaddr(&a4, 8, &na) == -EINVAL);
    CHECK(lx_sockaddr_to_netaddr(&a4, 1, &na) == -EINVAL);
    struct lx_sockaddr_in6 a6 = { .sin6_family = LX_AF_INET6, .sin6_port = htons(53) };
    a6.sin6_addr[15] = 1;
    CHECK(lx_sockaddr_to_netaddr(&a6, sizeof(a6), &na) == 0 && na.family == COSMO_AF_INET6 && na.port == 53 &&
          na.v6.s6_addr[15] == 1);
    uint16_t unix_family = 1;
    CHECK(lx_sockaddr_to_netaddr(&unix_family, 2, &na) == -EAFNOSUPPORT);
    /* sockaddr out */
    struct netaddr out4 = { .family = COSMO_AF_INET, .port = 40000, .v4 = 0x0100007f };
    unsigned char buf[32];
    memset(buf, 0xee, sizeof(buf));
    CHECK(lx_sockaddr_from_netaddr(&out4, buf, sizeof(buf)) == 16);
    struct lx_sockaddr_in back;
    memcpy(&back, buf, sizeof(back));
    CHECK(back.sin_family == LX_AF_INET && back.sin_port == htons(40000) && back.sin_addr == 0x0100007f && buf[16] == 0xee);
    memset(buf, 0xee, sizeof(buf));
    CHECK(lx_sockaddr_from_netaddr(&out4, buf, 4) == 16 && buf[4] == 0xee);   /* short buffer: prefix only */
    struct netaddr out6 = { .family = COSMO_AF_INET6, .port = 1 };
    out6.v6.s6_addr[0] = 0xfe;
    CHECK(lx_sockaddr_from_netaddr(&out6, buf, sizeof(buf)) == 28 && buf[8] == 0xfe && buf[28] == 0xee);

    /* dirents: two native records -> two linux_dirent64 records. The native
     * name starts at byte 12 (the packed header), not at sizeof(struct). */
    const size_t nh = offsetof(struct cosmo_dirent, name);
    CHECK(nh == 12);
    unsigned char in[128];
    memset(in, 0, sizeof(in));
    struct cosmo_dirent d1 = { .ino = 5, .reclen = 24, .type = COSMO_DT_REG, .namelen = 5 };
    memcpy(in, &d1, nh);
    memcpy(in + nh, "hello", 5);
    struct cosmo_dirent d2 = { .ino = 6, .reclen = 16, .type = COSMO_DT_DIR, .namelen = 1 };
    memcpy(in + 24, &d2, nh);
    memcpy(in + 24 + nh, ".", 1);
    unsigned char out[128];
    size_t o = lx_dirents_from_native(in, 40, out, sizeof(out));
    CHECK(o == 32 + 24);
    struct lx_dirent64 h1;
    memcpy(&h1, out, 19);
    CHECK(h1.d_ino == 5 && h1.d_reclen == 32 && h1.d_type == LX_DT_REG && memcmp(out + 19, "hello", 6) == 0);
    struct lx_dirent64 h2;
    memcpy(&h2, out + 32, 19);
    CHECK(h2.d_ino == 6 && h2.d_reclen == 24 && h2.d_type == LX_DT_DIR && out[32 + 19] == '.' && out[32 + 20] == 0);
    CHECK(lx_dirents_from_native(in, 40, out, 40) == 32);       /* the second record does not fit */
    d1.reclen = 8;                                                /* corrupt: shorter than the header */
    memcpy(in, &d1, nh);
    CHECK(lx_dirents_from_native(in, 40, out, sizeof(out)) == 0);
    CHECK(lx_dirent_type(COSMO_DT_CHR) == LX_DT_CHR && lx_dirent_type(COSMO_DT_FIFO) == LX_DT_FIFO &&
          lx_dirent_type(COSMO_DT_SOCK) == LX_DT_SOCK && lx_dirent_type(99) == LX_DT_UNKNOWN);

    /* prot */
    int p;
    CHECK(lx_prot(LX_PROT_READ | LX_PROT_WRITE, &p) == 0 && p == (COSMO_PROT_READ | COSMO_PROT_WRITE));
    CHECK(lx_prot(0x10, &p) < 0);

    if (g_failures) {
        printf("linux                         FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("linux                         ok\n");
    return 0;
}
