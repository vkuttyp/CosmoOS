/*
 * test_linux.c - Host test of the Linux personality's pure conversions
 * (docs/compat/linux/testing.md): open flags, stat, wait status,
 * sockaddr both ways, getdents64 records, PROT bits. ASan and UBSan.
 *
 * Besides direct checks, a recorded digest (roadmap 2.0 SA5, the
 * differential test of the Linux-syscall-argument family): 100,000
 * inputs from a seeded generator through every conversion lxconv
 * holds -- open flags under both architectures' encodings, PROT bits,
 * wait statuses, socket addresses in and out at every cut, the guest's
 * termios, getdents64 records from native ones of good and bad lengths.
 * The C gave it (lxconv.c); the CeeSafe (lxconv.cee) must give the same.
 * LINUX_DUMP=1 prints the lines; LINUX_BENCH=1 times the conversions.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <kernel/errno.h>

#include "convert.h"

/* After the kernel's headers: the host's define htons as a macro, which inet.h's functions would collide with. */
#include <stdlib.h>
#include <time.h>

static int g_failures;
#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (!(c)) {                                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                         \
            g_failures++;                                                                 \
        }                                                                                 \
    } while (0)

/* --- the recorded digest --------------------------------------------------- */

#define ROUNDS 100000
#define SEED 0x11C0FFEE5A520261ull
/* The digest of the C conversions (lxconv.c, 2026-10-31). */
#define LINUX_DIGEST 0x3a72da1935783ee6ull

/* x86-64's and AArch64's O_DIRECTORY, O_NOFOLLOW, O_LARGEFILE (linux_abi.h). */
static const unsigned O_BITS[2][3] = { { 040000, 0100000, 0400000 }, { 0200000, 0400000, 0100000 } };

static uint64_t g_rng;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

struct input {
    uint32_t word;
    uint8_t sa[32];
    size_t sa_len;
    size_t cap;
    struct lx_termios tio;
    uint8_t dirents[160];
    size_t dlen;
    size_t dcap;
};

/* Native getdents records of right and wrong lengths, names of every size. */
static void make_dirents(struct input *in)
{
    size_t ip = 0;
    memset(in->dirents, 0, sizeof(in->dirents));
    while (ip + 12 < sizeof(in->dirents) && next() % 5) {
        uint8_t namelen = (uint8_t)(next() % 20);
        size_t reclen = (12 + namelen + 7) & ~(size_t)7;
        if (next() % 6 == 0)
            reclen = (size_t)(next() % 48);
        if (ip + 12 > sizeof(in->dirents))
            break;
        uint64_t ino = next();
        memcpy(in->dirents + ip, &ino, 8);
        uint16_t rl = (uint16_t)reclen;
        memcpy(in->dirents + ip + 8, &rl, 2);
        in->dirents[ip + 10] = (uint8_t)(next() % 8);
        in->dirents[ip + 11] = next() % 8 ? namelen : (uint8_t)next();
        for (size_t k = 0; k < namelen && ip + 12 + k < sizeof(in->dirents); k++)
            in->dirents[ip + 12 + k] = (uint8_t)('a' + next() % 26);
        if (reclen == 0)
            break;
        ip += reclen;
    }
    in->dlen = next() % 4 ? (ip < sizeof(in->dirents) ? ip : sizeof(in->dirents)) : (size_t)(next() % sizeof(in->dirents));
    in->dcap = (size_t)(next() % 200);
}

static void make_input(struct input *in)
{
    in->word = (uint32_t)next();
    switch (next() % 4) {   /* flags and statuses near the meaningful values */
    case 0: in->word &= 0x7ffff; break;
    case 1: in->word &= 0x3ff; break;
    case 2: in->word = (uint32_t)(next() % 8); break;
    default: break;
    }
    for (size_t i = 0; i < sizeof(in->sa); i += 8) {
        uint64_t r = next();
        memcpy(in->sa + i, &r, 8);
    }
    uint16_t fam = (uint16_t)(next() % 3 == 0 ? 2 : next() % 2 ? 10 : next() % 16);
    memcpy(in->sa, &fam, 2);
    in->sa_len = (size_t)(next() % 33);
    in->cap = (size_t)(next() % 33);
    memset(&in->tio, 0, sizeof(in->tio));
    in->tio.c_iflag = (uint32_t)next();
    in->tio.c_oflag = (uint32_t)next();
    in->tio.c_cflag = (uint32_t)next();
    in->tio.c_lflag = (uint32_t)next();
    for (int i = 0; i < LX_NCCS; i++)
        in->tio.c_cc[i] = (uint8_t)next();
    make_dirents(in);
}

static uint64_t g_digest;
static bool g_dump;

static void emit(const char *line, int n)
{
    for (int i = 0; i < n; i++) {
        g_digest ^= (uint8_t)line[i];
        g_digest *= 0x100000001b3ull;
    }
    g_digest ^= '\n';
    g_digest *= 0x100000001b3ull;
    if (g_dump)
        printf("%s\n", line);
}

static void run_one(const struct input *in)
{
    char line[1024];
    int n = 0;
    for (int a = 0; a < 2; a++) {
        unsigned native = 0xdead;
        int rc = lx_open_flags_for(in->word, O_BITS[a][0], O_BITS[a][1], O_BITS[a][2], &native);
        n += snprintf(line + n, sizeof(line) - (size_t)n, "o%d:%x ", rc, rc == 0 ? native : 0);
    }
    int prot = 0;
    int prc = lx_prot(in->word, &prot);
    n += snprintf(line + n, sizeof(line) - (size_t)n, "p%d:%d w%d d%u ", prc, prc == 0 ? prot : 0,
                  lx_wait_status((int)in->word), lx_dirent_type((uint8_t)in->word));
    struct netaddr na;
    int src = lx_sockaddr_to_netaddr(in->sa, in->sa_len, &na);
    n += snprintf(line + n, sizeof(line) - (size_t)n, "s%d:%u:%u:", src, na.family, na.port);
    for (int i = 0; i < 16; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x", na.v6.s6_addr[i]);
    if (src == 0) {
        uint8_t out[32];
        memset(out, 0xa5, sizeof(out));
        size_t full = lx_sockaddr_from_netaddr(&na, out, in->cap);
        n += snprintf(line + n, sizeof(line) - (size_t)n, " f%zu:", full);
        for (size_t i = 0; i < sizeof(out); i++)
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x", out[i]);
    }
    struct cosmo_termios ct;
    memset(&ct, 0xa5, sizeof(ct));
    lx_termios_to_native(&ct, &in->tio);
    n += snprintf(line + n, sizeof(line) - (size_t)n, " t%x:%u:%u:%u", ct.modes, ct.vmin, ct.vtime, ct.reserved);
    uint8_t dout[200];
    memset(dout, 0xa5, sizeof(dout));
    size_t dn = lx_dirents_from_native(in->dirents, in->dlen, dout, in->dcap);
    n += snprintf(line + n, sizeof(line) - (size_t)n, " g%zu:", dn);
    for (size_t i = 0; i < dn; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x", dout[i]);
    CHECK(n > 0 && (size_t)n < sizeof(line));
    emit(line, n);
}

static void run_digest(void)
{
    g_dump = getenv("LINUX_DUMP") != NULL;
    g_digest = 0xcbf29ce484222325ull;
    g_rng = SEED;
    for (unsigned r = 0; r < ROUNDS; r++) {
        struct input in;
        make_input(&in);
        run_one(&in);
    }
    printf("  linux digest 0x%016llx over %u inputs\n", (unsigned long long)g_digest, ROUNDS);
    CHECK(g_digest == LINUX_DIGEST);
}

static volatile uint64_t g_sink;

/* LINUX_BENCH=1: every conversion on the same inputs, best of 9, ns per input. */
static int bench(void)
{
    struct input *ins = malloc(sizeof(*ins) * ROUNDS);
    if (ins == NULL)
        return 1;
    g_rng = SEED;
    for (unsigned r = 0; r < ROUNDS; r++)
        make_input(&ins[r]);
    double best = 1e30;
    for (int run = 0; run < 9; run++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t acc = 0;
        for (unsigned r = 0; r < ROUNDS; r++) {
            const struct input *in = &ins[r];
            unsigned native;
            acc += (unsigned)lx_open_flags_for(in->word, 040000, 0100000, 0400000, &native);
            int prot;
            acc += (unsigned)lx_prot(in->word, &prot);
            acc += (unsigned)lx_wait_status((int)in->word) + lx_dirent_type((uint8_t)in->word);
            struct netaddr na;
            if (lx_sockaddr_to_netaddr(in->sa, in->sa_len, &na) == 0) {
                uint8_t out[32];
                acc += lx_sockaddr_from_netaddr(&na, out, in->cap);
            }
            struct cosmo_termios ct;
            lx_termios_to_native(&ct, &in->tio);
            acc += ct.modes;
            uint8_t dout[200];
            acc += lx_dirents_from_native(in->dirents, in->dlen, dout, in->dcap);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        g_sink += acc;
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        if (ns < best)
            best = ns;
    }
    printf("linux bench: %.2f ns per input (best of 9 runs of %u)\n", best / ROUNDS, ROUNDS);
    free(ins);
    return 0;
}

int main(void)
{
    if (getenv("LINUX_BENCH") != NULL)
        return bench();
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
          n == (COSMO_O_RDWR | COSMO_O_APPEND | COSMO_O_EXCL | COSMO_O_DIRECTORY | COSMO_O_NONBLOCK));   /* O_NONBLOCK reaches the kernel since the named-pipes unit */
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

    run_digest();

    if (g_failures) {
        printf("linux                         FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("linux                         ok\n");
    return 0;
}
