/*
 * test_acpi.c - Host test of the ACPI table walker's decoders
 * (drivers/acpi/acpi_parse; roadmap 2.0 SA5, the virtq/USB/ACPI family):
 * a recorded digest over seeded firmware tables -- RSDPs of both
 * revisions with right and wrong signatures and checksums, root tables of
 * both widths, and MADTs of every entry type this kernel reads (and some
 * it does not) at every length around each type's own, past the limits
 * of CPUs and IOAPICs, short, and stopped by an entry whose length is
 * wrong. Every result is a line; the lines' FNV-1a digest is recorded
 * below. The C gave it (2026-11-01), and the CeeSafe that replaced it
 * gives the same. ACPI_DUMP=1 prints the lines; ACPI_BENCH=1 times the
 * MADT decoder. ASan and UBSan.
 */

#include "harness.h"

#include "acpi_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROUNDS 20000
#define SEED 0xC05F5FA2026B0006ull
/* The digest of the C decoders (acpi_parse.c, 2026-11-01). */
#define ACPI_DIGEST 0x117ff5b066c8c9abull

static uint64_t g_rng;
static uint64_t g_digest;
static bool g_dump;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void emit(const char *line, int n)
{
    EXPECT(n > 0 && n < 512);
    for (int i = 0; i < n; i++) {
        g_digest ^= (uint8_t)line[i];
        g_digest *= 0x100000001b3ull;
    }
    g_digest ^= '\n';
    g_digest *= 0x100000001b3ull;
    if (g_dump)
        printf("%s\n", line);
}

static void fill(uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)next();
}

/* Make `b[at]` the byte that brings `n` bytes' sum to zero. */
static void seal(uint8_t *b, size_t n, size_t at)
{
    b[at] = 0;
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++)
        s = (uint8_t)(s + b[i]);
    b[at] = (uint8_t)-s;
}

static void run_rsdp(void)
{
    _Alignas(8) uint8_t b[sizeof(struct acpi_rsdp)];
    struct acpi_rsdp *r = (struct acpi_rsdp *)b;
    fill(b, sizeof(b));
    if (next() % 8)
        memcpy(r->signature, "RSD PTR ", 8);
    r->revision = (uint8_t)(next() % 2 ? 2 : next() % 4);
    if (next() % 4 == 0)
        r->xsdt_address = 0;
    static const uint32_t lens[] = { 0, 19, 20, 21, 35, 36, 37, 0xffffffffu };
    r->length = next() % 2 ? 36 : lens[next() % 8];
    if (next() % 4)
        seal(b, 20, offsetof(struct acpi_rsdp, checksum));
    if (next() % 4) {
        size_t n = r->length < sizeof(*r) ? r->length : sizeof(*r);
        if (n > 20)   /* past the first checksum: the extended one makes it zero */
            seal(b, n, offsetof(struct acpi_rsdp, ext_checksum));
    }
    paddr_t root = 0xa5a5a5a5a5a5a5a5ull;
    unsigned width = 0xa5;
    int v = acpi_rsdp_decode(r, &root, &width);
    char line[128];
    int n = snprintf(line, sizeof(line), "p%d %016llx %u %02x", v, v == 0 ? (unsigned long long)root : 0ull,
                     v == 0 ? width : 0, acpi_checksum(b, next() % (sizeof(b) + 1)));
    emit(line, n);
}

static void run_root(void)
{
    _Alignas(8) static uint8_t b[36 + 8 * 40 + 8];
    struct acpi_sdt_header *h = (struct acpi_sdt_header *)b;
    fill(b, sizeof(b));
    for (size_t i = 36; i < sizeof(b); i++)
        if (next() % 4 == 0)
            b[i] = 0;   /* now and then a zero address */
    unsigned width = next() % 2 ? 8 : 4;
    h->length = (uint32_t)(36 + next() % (sizeof(b) - 36 + 1));
    size_t count = acpi_root_count(h, width);
    char line[128];
    int n = snprintf(line, sizeof(line), "t%u %zu", width, count);
    emit(line, n);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < count; i++)
        hash = (hash ^ acpi_root_entry(h, width, i)) * 0x100000001b3ull;
    n = snprintf(line, sizeof(line), " %016llx", (unsigned long long)hash);
    emit(line, n);
}

#define MADT_MAX 2048u

/* One MADT entry of a type this kernel reads (or not), its length its
 * type's own most of the time; false when it would not fit. */
static bool madt_entry(uint8_t *b, unsigned *off)
{
    static const struct { uint8_t type, len; } kinds[] = {
        { 0, 8 }, { 1, 12 }, { 2, 10 }, { 5, 12 }, { 9, 16 }, { 11, 80 }, { 11, 76 },
        { 12, 24 }, { 13, 24 }, { 14, 16 }, { 15, 20 }, { 3, 8 }, { 4, 6 }, { 0x7f, 12 },
    };
    unsigned k = (unsigned)(next() % (sizeof(kinds) / sizeof(kinds[0])));
    unsigned len = kinds[k].len;
    switch (next() % 16) {
    case 0: len = len - 1; break;
    case 1: len = len + 1 + (unsigned)(next() % 8); break;
    case 2: len = (unsigned)(next() % 256); break;
    default: break;
    }
    if (*off + (len < 2 ? 2 : len) > MADT_MAX)
        return false;
    fill(b + *off, len < 2 ? 2 : len);
    b[*off] = kinds[k].type;
    b[*off + 1] = (uint8_t)len;
    if (kinds[k].type == 0 || kinds[k].type == 9 || kinds[k].type == 11) {
        /* CPUs enabled or online-capable most of the time (flags at 4, 8 or 12) */
        unsigned at = kinds[k].type == 0 ? 4 : kinds[k].type == 9 ? 8 : 12;
        if (at + 4 <= len && next() % 4)
            b[*off + at] |= 1;
    }
    *off += len < 2 ? 2 : len;
    return true;
}

static uint64_t state_hash(const struct acpi_madt_state *st)
{
    uint64_t h = 0xcbf29ce484222325ull;
#define MIX(v) (h = (h ^ (uint64_t)(v)) * 0x100000001b3ull)
    MIX(st->lapic_base);
    MIX(st->cpu_count);
    for (size_t i = 0; i < st->cpu_count && i < ACPI_MAX_CPUS; i++) {
        MIX(st->cpus[i].apic_id);
        MIX(st->cpus[i].acpi_id);
        MIX(st->cpus[i].x2apic);
    }
    MIX(st->ioapic_count);
    for (size_t i = 0; i < st->ioapic_count && i < ACPI_MAX_IOAPICS; i++) {
        MIX(st->ioapics[i].id);
        MIX(st->ioapics[i].address);
        MIX(st->ioapics[i].gsi_base);
    }
    MIX(st->override_count);
    for (size_t i = 0; i < st->override_count && i < ACPI_MAX_OVERRIDES; i++) {
        MIX(st->overrides[i].bus);
        MIX(st->overrides[i].source);
        MIX(st->overrides[i].gsi);
        MIX(st->overrides[i].flags);
    }
    const struct acpi_gic *g = &st->gic;
    MIX(g->version);
    MIX(g->gicd_base);
    MIX(g->gicc_base);
    MIX(g->v2m_base);
    MIX(g->v2m_spi_base);
    MIX(g->v2m_spi_count);
    MIX(g->gicr_base);
    MIX(g->gicr_length);
    MIX(g->gicc_gicr_base);
    MIX(g->its_base);
    MIX(g->its_id);
    MIX(g->gicv_base);
    MIX(g->gich_base);
    MIX(g->maint_gsiv);
#undef MIX
    return h;
}

static void gen_madt(uint8_t *b)
{
    memset(b, 0, MADT_MAX);
    unsigned off = sizeof(struct acpi_madt);
    fill(b, off);
    unsigned r = (unsigned)(next() % 8);
    unsigned n = (unsigned)(r < 2 ? 60 + next() % 20 : r == 2 ? 6 + next() % 8 : next() % 24);
    for (unsigned i = 0; i < n; i++) {
        if (r < 3) {   /* past 64 CPUs (LAPIC entries, enabled), or past 8 IOAPICs, now and then */
            unsigned len = r < 2 ? 8 : 12;
            if (off + len > MADT_MAX)
                break;
            fill(b + off, len);
            b[off] = r < 2 ? 0 : 1;
            b[off + 1] = (uint8_t)len;
            b[off + 4] |= 1;
            off += len;
        } else if (!madt_entry(b, &off)) {
            break;
        }
    }
    struct acpi_madt *m = (struct acpi_madt *)b;
    unsigned len = off;
    switch (next() % 16) {
    case 0: len = (unsigned)(next() % 44); break;   /* shorter than its fixed fields */
    case 1: len = off - (unsigned)(next() % 8); break;
    default: break;
    }
    m->hdr.length = len;
}

static void run_madt(void)
{
    _Alignas(8) static uint8_t b[MADT_MAX];
    gen_madt(b);
    static struct acpi_madt_state st;
    memset(&st, 0, sizeof(st));
    acpi_madt_decode((const struct acpi_madt *)b, &st);
    char line[256];
    int n = snprintf(line, sizeof(line), "m%d %d %u %u %u %d %016llx", st.too_short, st.stopped, st.bad_entry_len,
                     st.extra_cpus, st.extra_ioapics, st.gic_present, (unsigned long long)state_hash(&st));
    emit(line, n);
}

static void test_digest(void)
{
    g_dump = getenv("ACPI_DUMP") != NULL;
    g_digest = 0xcbf29ce484222325ull;
    g_rng = SEED;
    for (unsigned r = 0; r < ROUNDS; r++) {
        run_rsdp();
        run_root();
        run_madt();
    }
    printf("  acpi digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == ACPI_DIGEST);
}

/* QEMU q35's MADT, as the boot log describes it: a LAPIC, an IOAPIC at
 * 0xfec00000 and five overrides. */
static void test_q35(void)
{
    _Alignas(8) static uint8_t b[256];
    memset(b, 0, sizeof(b));
    unsigned off = sizeof(struct acpi_madt);
    struct acpi_madt *m = (struct acpi_madt *)b;
    memcpy(m->hdr.signature, "APIC", 4);
    m->lapic_address = 0xfee00000u;
    static const uint8_t lapic[] = { 0, 8, 0, 0, 1, 0, 0, 0 };
    memcpy(b + off, lapic, sizeof(lapic));
    off += sizeof(lapic);
    static const uint8_t ioapic[] = { 1, 12, 0, 0, 0x00, 0x00, 0xc0, 0xfe, 0, 0, 0, 0 };
    memcpy(b + off, ioapic, sizeof(ioapic));
    off += sizeof(ioapic);
    static const uint8_t isos[5] = { 0, 5, 9, 10, 11 };
    for (unsigned i = 0; i < 5; i++) {
        uint8_t iso[10] = { 2, 10, 0, isos[i], i == 0 ? 2 : isos[i], 0, 0, 0, i == 2 ? 0x0d : 0, 0 };
        memcpy(b + off, iso, sizeof(iso));
        off += sizeof(iso);
    }
    m->hdr.length = off;
    static struct acpi_madt_state st;
    memset(&st, 0, sizeof(st));
    acpi_madt_decode(m, &st);
    EXPECT(!st.too_short && !st.stopped && st.lapic_base == 0xfee00000u);
    EXPECT(st.cpu_count == 1 && st.cpus[0].apic_id == 0 && !st.cpus[0].x2apic);
    EXPECT(st.ioapic_count == 1 && st.ioapics[0].address == 0xfec00000u);
    EXPECT(st.override_count == 5 && st.overrides[0].gsi == 2 && st.overrides[2].flags == 0x0d);
    EXPECT(!st.gic_present);
}

static volatile uint64_t g_sink;

/* ACPI_BENCH=1: the MADT decoder alone on 500 tables made beforehand (best of 15). */
static int bench(void)
{
    enum { N = 500 };
    uint8_t *bs = aligned_alloc(8, (size_t)N * MADT_MAX);
    struct acpi_madt_state *st = malloc(sizeof(*st));
    if (bs == NULL || st == NULL)
        return 1;
    g_rng = SEED;
    for (unsigned i = 0; i < N; i++)
        gen_madt(bs + (size_t)i * MADT_MAX);
    double best = 1e30;
    for (int run = 0; run < 15; run++) {
        struct timespec t0, t1;
        uint64_t acc = 0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < N; i++) {
            memset(st, 0, sizeof(*st));
            acpi_madt_decode((const struct acpi_madt *)(bs + (size_t)i * MADT_MAX), st);
            acc += st->cpu_count + st->gic.gicd_base;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        g_sink += acc;
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        if (ns < best)
            best = ns;
    }
    printf("acpi bench: %.2f ns per MADT (best of 15 runs of %u)\n", best / N, N);
    free(bs);
    free(st);
    return 0;
}

static const struct host_test tests[] = {
    { "acpi digest", test_digest },
    { "acpi q35 madt", test_q35 },
};

int main(void)
{
    if (getenv("ACPI_BENCH") != NULL)
        return bench();
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
