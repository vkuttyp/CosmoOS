/*
 * ksym.c - The kernel's exported symbol table and the shared sorted
 * lookup used for module export tables.
 *
 * .ksymtab is read-only, so the index is an array of pointers sorted by
 * name (heap sort: no recursion, no allocation beyond the array).
 */

#include <kernel/kmalloc.h>
#include <kernel/ksym.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/string.h>

extern const struct ksym __ksymtab_start[];
extern const struct ksym __ksymtab_end[];

static const struct ksym **g_index;
static size_t g_count;

static void sift_down(const struct ksym **v, size_t start, size_t end)
{
    size_t root = start;
    while (2 * root + 1 < end) {
        size_t child = 2 * root + 1;
        if (child + 1 < end && strcmp(v[child]->name, v[child + 1]->name) < 0)
            child++;
        if (strcmp(v[root]->name, v[child]->name) >= 0)
            return;
        const struct ksym *t = v[root];
        v[root] = v[child];
        v[child] = t;
        root = child;
    }
}

void ksym_sort(const struct ksym **v, size_t n)
{
    if (n < 2)
        return;
    for (size_t i = n / 2; i-- > 0;)
        sift_down(v, i, n);
    for (size_t end = n - 1; end > 0; end--) {
        const struct ksym *t = v[0];
        v[0] = v[end];
        v[end] = t;
        sift_down(v, 0, end);
    }
}

uintptr_t ksym_search(const struct ksym *const *v, size_t n, const char *name)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(v[mid]->name, name);
        if (c == 0)
            return v[mid]->addr;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

void ksym_init(void)
{
    KASSERT(g_index == NULL);
    size_t n = (size_t)(__ksymtab_end - __ksymtab_start);
    const struct ksym **idx = kmalloc(n * sizeof(*idx), 0);
    if (idx == NULL)
        panic("ksym: cannot allocate export index for %zu symbols", n);
    for (size_t i = 0; i < n; i++) {
        if (__ksymtab_start[i].name == NULL || __ksymtab_start[i].addr == 0)
            panic("ksym: export %zu is malformed", i);
        idx[i] = &__ksymtab_start[i];
    }
    ksym_sort(idx, n);
    for (size_t i = 1; i < n; i++) {
        if (strcmp(idx[i - 1]->name, idx[i]->name) == 0)
            panic("ksym: symbol %s exported twice", idx[i]->name);
    }
    g_index = idx;
    g_count = n;
    kinfo("ksym: %zu exported symbols (module ABI v%u)", n, COSMO_MODULE_ABI_VERSION);
}

uintptr_t ksym_lookup(const char *name)
{
    if (g_index == NULL)
        return 0;
    return ksym_search(g_index, g_count, name);
}

size_t ksym_count(void)
{
    return g_count;
}

const struct ksym *ksym_entry(size_t sorted_index)
{
    return sorted_index < g_count ? g_index[sorted_index] : NULL;
}
