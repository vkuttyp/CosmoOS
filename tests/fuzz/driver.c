/*
 * driver.c - Portable fuzz driver (docs/verification/design.md, "Driver").
 *
 * Runs a target over its programmatic seeds, over every file in an
 * optional corpus directory, and then over FUZZ_RUNS mutations of those
 * inputs drawn with a seeded xorshift generator, so a run is reproducible
 * from its seed and count. Used where libFuzzer is unavailable (Apple
 * clang) and for the bounded run in CI; FUZZ_ENGINE=libfuzzer links the
 * same target without this file.
 *
 *   driver [-runs N] [-seed S] [-max_len L] [-out DIR] [-verbose] [corpus_dir | file ...]
 *
 * The code under test's klog lines are dropped unless -verbose is given.
 *
 * A file argument that is a regular file is replayed once (a crash
 * reproducer). Inputs that crash are written to DIR/crash-<n> before the
 * sanitizer's report ends the process, by an atexit hook on the last
 * input tried.
 */

#include "fuzz.h"

#include <dirent.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_INPUTS 4096

struct input {
    uint8_t *data;
    size_t len;
};

static struct input g_inputs[MAX_INPUTS];
static unsigned g_nr_inputs;
static size_t g_max_len = 65536;
static const char *g_out_dir = ".";
static unsigned long g_iteration;
static uint8_t *g_current;
static size_t g_current_len;
static volatile int g_in_target;

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void)
{
    /* xorshift64* */
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}

static unsigned rnd_below(unsigned n)
{
    return n ? (unsigned)(rnd() % n) : 0;
}

void fuzz_fail(const char *file, int line, const char *expr)
{
    fprintf(stderr, "fuzz: target assertion failed: %s (%s:%d) on iteration %lu\n", expr, file, line, g_iteration);
    abort();
}

/* Weak defaults; a target that needs either defines its own. */
__attribute__((weak)) int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

__attribute__((weak)) size_t fuzz_max_len(void)
{
    return 0;
}

static void add_input(const uint8_t *data, size_t len)
{
    if (g_nr_inputs == MAX_INPUTS)
        return;
    struct input *in = &g_inputs[g_nr_inputs++];
    in->data = malloc(len ? len : 1);
    memcpy(in->data, data, len);
    in->len = len;
}

static void load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "fuzz: cannot open %s: %s\n", path, strerror(errno));
        exit(2);
    }
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 4096;
            buf = realloc(buf, cap);
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        if (n == 0)
            break;
        len += n;
    }
    fclose(f);
    add_input(buf, len);
    free(buf);
}

static void load_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        fprintf(stderr, "fuzz: cannot open corpus %s: %s\n", dir, strerror(errno));
        exit(2);
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
            load_file(path);
    }
    closedir(d);
}

/* On a crash the sanitizer calls abort; save the input first so the
 * failure reproduces with `driver <file>`. */
static void save_current(void)
{
    if (!g_in_target || g_current == NULL)
        return;
    char path[4096];
    snprintf(path, sizeof(path), "%s/crash-%lu", g_out_dir, g_iteration);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(g_current, 1, g_current_len, f);
        fclose(f);
        fprintf(stderr, "fuzz: crashing input (%zu bytes) saved to %s\n", g_current_len, path);
    }
}

static void on_signal(int sig)
{
    save_current();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void run_one(const uint8_t *data, size_t len)
{
    g_current = (uint8_t *)data;
    g_current_len = len;
    g_in_target = 1;
    LLVMFuzzerTestOneInput(data, len);
    g_in_target = 0;
    g_iteration++;
}

static const uint32_t k_interesting[] = { 0, 1, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
                                         0x7fffffff, 0x80000000u, 0xffffffffu, 4096, 4095, 4097, 512, 64 };

/* One mutation of `in` into `out` (cap `cap`); returns the new length. */
static size_t mutate(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    memcpy(out, in, len);
    unsigned n = 1 + rnd_below(4);
    for (unsigned m = 0; m < n; m++) {
        switch (rnd_below(9)) {
        case 0:   /* bit flip */
            if (len)
                out[rnd_below((unsigned)len)] ^= (uint8_t)(1u << rnd_below(8));
            break;
        case 1:   /* byte set */
            if (len)
                out[rnd_below((unsigned)len)] = (uint8_t)rnd();
            break;
        case 2: { /* interesting value, 1/2/4/8 bytes */
            unsigned w = 1u << rnd_below(4);
            if (len >= w) {
                size_t off = rnd_below((unsigned)(len - w + 1));
                uint64_t v = k_interesting[rnd_below(sizeof(k_interesting) / sizeof(k_interesting[0]))];
                if (w == 8 && rnd_below(2))
                    v |= (uint64_t)k_interesting[rnd_below(18)] << 32;
                memcpy(out + off, &v, w);
            }
            break;
        }
        case 3: { /* insert a chunk */
            size_t ins = 1 + rnd_below(64);
            if (len + ins <= cap) {
                size_t off = rnd_below((unsigned)(len + 1));
                memmove(out + off + ins, out + off, len - off);
                for (size_t i = 0; i < ins; i++)
                    out[off + i] = (uint8_t)rnd();
                len += ins;
            }
            break;
        }
        case 4: { /* delete a chunk */
            if (len > 1) {
                size_t del = 1 + rnd_below((unsigned)(len < 64 ? len - 1 : 64));
                size_t off = rnd_below((unsigned)(len - del + 1));
                memmove(out + off, out + off + del, len - off - del);
                len -= del;
            }
            break;
        }
        case 5: { /* duplicate a chunk */
            if (len && len < cap) {
                size_t dup = 1 + rnd_below((unsigned)(len < 64 ? len : 64));
                if (dup > cap - len)
                    dup = cap - len;
                size_t src = rnd_below((unsigned)(len - dup + 1));
                size_t off = rnd_below((unsigned)(len + 1));
                uint8_t tmp[64];
                memcpy(tmp, out + src, dup);
                memmove(out + off + dup, out + off, len - off);
                memcpy(out + off, tmp, dup);
                len += dup;
            }
            break;
        }
        case 6:   /* truncate */
            if (len)
                len = rnd_below((unsigned)len + 1);
            break;
        case 7: { /* extend with noise */
            size_t ext = 1 + rnd_below(256);
            if (len + ext > cap)
                ext = cap - len;
            for (size_t i = 0; i < ext; i++)
                out[len + i] = (uint8_t)rnd();
            len += ext;
            break;
        }
        default: { /* splice with another input */
            if (g_nr_inputs > 1) {
                const struct input *o = &g_inputs[rnd_below(g_nr_inputs)];
                if (o->len && len) {
                    size_t at = rnd_below((unsigned)len);
                    size_t from = rnd_below((unsigned)o->len);
                    size_t cnt = o->len - from;
                    if (at + cnt > cap)
                        cnt = cap - at;
                    memcpy(out + at, o->data + from, cnt);
                    if (at + cnt > len)
                        len = at + cnt;
                }
            }
            break;
        }
        }
    }
    return len;
}

/* The host harness's log threshold, when a target links it (a weak
 * definition so targets without the harness link too). The driver raises
 * it past KLOG_ERROR: a rejected input's diagnostics are the expected
 * outcome, twenty thousand times over. -verbose keeps them. */
int harness_klog_min __attribute__((weak)) = 2;

int main(int argc, char **argv)
{
    harness_klog_min = 4;
    unsigned long runs = 20000;
    uint64_t seed = 1;
    const char *paths[64];
    unsigned nr_paths = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-runs") == 0 && i + 1 < argc)
            runs = strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "-seed") == 0 && i + 1 < argc)
            seed = strtoull(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "-max_len") == 0 && i + 1 < argc)
            g_max_len = strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "-out") == 0 && i + 1 < argc)
            g_out_dir = argv[++i];
        else if (strcmp(argv[i], "-verbose") == 0)
            harness_klog_min = 2;   /* KLOG_WARN: the code under test's own diagnostics */
        else if (nr_paths < 64)
            paths[nr_paths++] = argv[i];
    }
    g_rng ^= seed * 0x9E3779B97F4A7C15ull;
    if (g_rng == 0)
        g_rng = 1;
    LLVMFuzzerInitialize(&argc, &argv);
    if (fuzz_max_len() > g_max_len)
        g_max_len = fuzz_max_len();
    signal(SIGABRT, on_signal);
    signal(SIGSEGV, on_signal);
    signal(SIGBUS, on_signal);
    atexit(save_current);

    /* Seeds. */
    uint8_t *buf = malloc(g_max_len);
    for (unsigned i = 0;; i++) {
        size_t n = fuzz_seed(i, buf, g_max_len);
        if (n == 0)
            break;
        add_input(buf, n);
    }
    unsigned nr_seeds = g_nr_inputs;
    bool replay_only = false;
    for (unsigned i = 0; i < nr_paths; i++) {
        struct stat st;
        if (stat(paths[i], &st) == 0 && S_ISDIR(st.st_mode))
            load_dir(paths[i]);
        else {
            load_file(paths[i]);
            replay_only = true;
        }
    }
    if (g_nr_inputs == 0) {
        fprintf(stderr, "fuzz: no seeds and no corpus\n");
        return 2;
    }

    /* Every seed and corpus entry once. */
    for (unsigned i = 0; i < g_nr_inputs; i++)
        run_one(g_inputs[i].data, g_inputs[i].len);
    if (replay_only) {
        printf("fuzz: replayed %u input(s) without failure\n", g_nr_inputs);
        return 0;
    }

    /* Mutations. */
    uint8_t *mut = malloc(g_max_len);
    for (unsigned long r = 0; r < runs; r++) {
        const struct input *in = &g_inputs[rnd_below(g_nr_inputs)];
        size_t len = in->len <= g_max_len ? in->len : g_max_len;
        len = mutate(in->data, len, mut, g_max_len);
        run_one(mut, len);
    }
    printf("fuzz: %u seed(s), %u corpus input(s), %lu mutation(s): no failure\n", nr_seeds, g_nr_inputs - nr_seeds,
           runs);
    free(mut);
    free(buf);
    for (unsigned i = 0; i < g_nr_inputs; i++)
        free(g_inputs[i].data);
    return 0;
}
