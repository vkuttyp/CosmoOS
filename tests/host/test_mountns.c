/*
 * test_mountns.c - Mount namespaces (kernel-services/vfs/mountns.c) on the host: roadmap 2.0 SA7. A digest recorded
 * from the C over random histories: mounts made and seen by some namespaces, namespaces copied from one another while
 * the allocator fails part-way, references taken and dropped until a namespace dies and takes the mounts only it could
 * see with it, newest first, and visibility asked of every pair. Every lock taken and released, every mount detached
 * and orphaned, every result, and each namespace's view and each mount's viewers, in order, are in the digest; every
 * reference allocated is given back.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mountns.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include "vfs_internal.h"

/* The C library's headers are not included: their pid_t is not the kernel's. */
void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void free(void *p);
int printf(const char *fmt, ...);

/* The digest of the C (mountns.c, 2026-11-07), which the CeeSafe port gives. */
#define MOUNTNS_DIGEST 0x20d2c61ff98c5c64ull
#define ROUNDS 6000u
#define SEED 0x3A7C0FFEE2026117ull
#define NMOUNTS 24u
#define NVNODES 6u
#define NNS 12u

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static unsigned pick(unsigned n) { return (unsigned)(next() % n); }

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void note(uint32_t which, uint64_t value)
{
    mix(&which, sizeof(which));
    mix(&value, sizeof(value));
}

size_t (strlcpy)(char *dst, const char *src, size_t size)   /* parenthesised: a fortified libc makes strlcpy a macro */
{
    size_t n = strlen(src);
    if (size > 0) {
        size_t k = n < size - 1 ? n : size - 1;
        memcpy(dst, src, k);
        dst[k] = '\0';
    }
    return n;
}

/* ---- the allocator: counted, filled with a pattern; an allocation fails when the countdown reaches it ---- */

static long g_live;
static int g_fail_in = -1;

void *kmalloc(size_t size, unsigned flags)
{
    note(1, size);
    note(2, flags);
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(3, 0);
        return NULL;
    }
    void *p = (flags & KMEM_ZERO) ? calloc(1, size) : malloc(size);
    EXPECT(p != NULL);
    if (!(flags & KMEM_ZERO))
        memset(p, 0xA5, size);
    g_live++;
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the mount table, the mountpoints and their locks ---- */

struct mutex g_mounts_lock;
struct list_node g_mounts;
struct mount *g_root_mount;

static struct mount g_mnt[NMOUNTS];
static bool g_mnt_live[NMOUNTS];
static struct vnode g_vn[NVNODES];
static bool g_held[NVNODES + 1];   /* 0: g_mounts_lock; 1..: the vnodes' locks */

static unsigned lock_index(struct mutex *m)
{
    if (m == &g_mounts_lock)
        return 0;
    for (unsigned i = 0; i < NVNODES; i++)
        if (m == &g_vn[i].lock)
            return i + 1;
    EXPECT(false);
    return 0;
}

void mutex_lock(struct mutex *m)
{
    unsigned i = lock_index(m);
    EXPECT(!g_held[i]);
    if (i > 0)   /* a mountpoint's lock nests in nothing but g_mounts_lock, and in no other vnode's */
        for (unsigned j = 1; j <= NVNODES; j++)
            EXPECT(!g_held[j]);
    g_held[i] = true;
    note(4, i);
}

void mutex_unlock(struct mutex *m)
{
    unsigned i = lock_index(m);
    EXPECT(g_held[i]);
    g_held[i] = false;
    note(5, i);
}

static unsigned mount_index(struct mount *mnt)
{
    EXPECT(mnt >= g_mnt && mnt < g_mnt + NMOUNTS);
    return (unsigned)(mnt - g_mnt);
}

void vfs_mount_detach(struct mount *mnt)
{
    unsigned i = mount_index(mnt);
    EXPECT(g_held[0] && g_mnt_live[i] && mnt != g_root_mount);
    EXPECT(list_empty(&mnt->ns_refs));
    note(6, i);
    list_remove(&mnt->link);
}

void vfs_mount_orphaned(struct mount *mnt)
{
    unsigned i = mount_index(mnt);
    for (unsigned j = 0; j <= NVNODES; j++)
        EXPECT(!g_held[j]);
    note(7, i);
    g_mnt_live[i] = false;
}

/* ---- the current process ---- */

static struct process g_proc;
static struct process *g_current;

struct process *process_current(void) { return g_current; }

/* ---- the namespaces the test holds, and the references it holds on each ---- */

static struct mount_ns *g_ns[NNS];
static uint32_t g_ns_refs[NNS];

/* A mount made where `ns` sees it, as vfs.c makes one: its reference on both lists. */
static void make_mount(void)
{
    unsigned i = 1 + pick(NMOUNTS - 1);
    if (g_mnt_live[i])
        return;
    unsigned holders[NNS], n = 0;
    for (unsigned k = 0; k < NNS; k++)
        if (g_ns[k] && pick(3) == 0)
            holders[n++] = k;
    if (n == 0)
        return;
    struct mount *mnt = &g_mnt[i];
    memset(mnt, 0, sizeof(*mnt));
    mnt->mountpoint = &g_vn[pick(NVNODES)];
    list_init(&mnt->ns_refs);
    list_push_back(&g_mounts, &mnt->link);
    g_mnt_live[i] = true;
    note(8, i);
    for (unsigned h = 0; h < n; h++) {
        struct mount_ns_ref *ref = calloc(1, sizeof(*ref));
        EXPECT(ref != NULL);
        g_live++;
        ref->mnt = mnt;
        ref->ns = g_ns[holders[h]];
        unsigned len = 1 + pick(40);
        for (unsigned c = 0; c < len; c++)
            ref->path[c] = (char)('a' + pick(26));
        ref->path[0] = '/';
        list_push_back(&mnt->ns_refs, &ref->mnt_link);
        list_push_back(&ref->ns->mounts, &ref->ns_link);
        note(9, holders[h]);
    }
}

static unsigned free_slot(void)
{
    for (unsigned k = 1; k < NNS; k++)
        if (g_ns[k] == NULL)
            return k;
    return 0;
}

static void note_view(void)
{
    for (unsigned k = 0; k < NNS; k++) {
        if (g_ns[k] == NULL)
            continue;
        note(10, k);
        note(11, g_ns[k]->id);
        note(12, g_ns[k]->refs);
        const struct mount_ns_ref *r;
        list_for_each_entry(r, &g_ns[k]->mounts, ns_link) {
            EXPECT(r->ns == g_ns[k]);
            note(13, mount_index(r->mnt));
            mix(r->path, strlen(r->path));
        }
    }
    for (unsigned i = 0; i < NMOUNTS; i++) {
        if (!g_mnt_live[i])
            continue;
        const struct mount_ns_ref *r;
        list_for_each_entry(r, &g_mnt[i].ns_refs, mnt_link) {
            unsigned k = 0;
            while (k < NNS && g_ns[k] != r->ns)
                k++;
            EXPECT(r->mnt == &g_mnt[i]);
            note(14, k);
        }
    }
}

static void test_histories(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    list_init(&g_mounts);
    g_mnt[0].mountpoint = &g_vn[0];
    list_init(&g_mnt[0].ns_refs);
    list_push_back(&g_mounts, &g_mnt[0].link);
    g_mnt_live[0] = true;
    g_root_mount = &g_mnt[0];
    g_ns[0] = mountns_initial();
    g_ns_refs[0] = 1;
    note(15, g_ns[0]->id);

    EXPECT(mountns_get(NULL) == NULL);
    mountns_put(NULL);
    mountns_put(mountns_initial());   /* the initial namespace is never freed: a put is nothing */
    EXPECT(mountns_initial()->refs == 1);

    for (unsigned round = 0; round < ROUNDS; round++) {
        unsigned op = pick(10);
        if (op <= 2) {
            make_mount();
        } else if (op <= 4) {
            unsigned parent = pick(NNS);
            unsigned slot = free_slot();
            if (g_ns[parent] == NULL || slot == 0)
                continue;
            struct mount_ns *out = (struct mount_ns *)(uintptr_t)0x5;
            g_fail_in = pick(3) == 0 ? (int)pick(8) : -1;
            int rc = mountns_create(g_ns[parent], &out);
            g_fail_in = -1;
            note(16, (uint64_t)(int64_t)rc);
            if (rc == 0) {
                g_ns[slot] = out;
                g_ns_refs[slot] = 1;
                note(17, slot);
            } else {
                EXPECT(out == (struct mount_ns *)(uintptr_t)0x5);
            }
        } else if (op == 5) {
            unsigned k = pick(NNS);
            if (g_ns[k] == NULL)
                continue;
            EXPECT(mountns_get(g_ns[k]) == g_ns[k]);
            g_ns_refs[k]++;
            note(18, g_ns[k]->refs);
        } else if (op <= 7) {
            unsigned k = 1 + pick(NNS - 1);
            if (g_ns[k] == NULL)
                continue;
            struct mount_ns *ns = g_ns[k];
            if (--g_ns_refs[k] == 0)
                g_ns[k] = NULL;
            mountns_put(ns);
            note(19, k);
        } else if (op == 8) {
            for (unsigned k = 0; k < NNS; k++)
                for (unsigned i = 0; i < NMOUNTS; i++)
                    if (g_ns[k] && g_mnt_live[i])
                        note(20, mountns_sees(g_ns[k], &g_mnt[i]));
        } else {
            unsigned w = pick(3);
            unsigned k = pick(NNS);
            g_current = w == 0 ? NULL : &g_proc;
            g_proc.mntns = w == 2 ? g_ns[k] : NULL;
            struct mount_ns *cur = mountns_current();
            note(21, cur == mountns_initial());
            note(22, cur == g_proc.mntns);
        }
        if (round % 16 == 0)
            note_view();
    }
    note_view();

    /* Every namespace dropped: what only they could see goes with them; the initial namespace's view stays. */
    for (unsigned k = 1; k < NNS; k++)
        while (g_ns[k] != NULL) {
            struct mount_ns *ns = g_ns[k];
            if (--g_ns_refs[k] == 0)
                g_ns[k] = NULL;
            mountns_put(ns);
        }
    note_view();
    struct mount_ns_ref *ref, *tmp;
    list_for_each_entry_safe(ref, tmp, &mountns_initial()->mounts, ns_link) {
        list_remove(&ref->ns_link);
        list_remove(&ref->mnt_link);
        kfree(ref);
    }
    note(23, (uint64_t)g_live);

    printf("mountns digest 0x%016llx\n", (unsigned long long)g_digest);
    EXPECT(g_live == 0);
    EXPECT(g_digest == MOUNTNS_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "mountns_histories", test_histories },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
