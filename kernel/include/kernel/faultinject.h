/*
 * faultinject.h - Fault injection for debug builds
 * (docs/verification/design.md, "Fault injection").
 *
 * A rule per kind: fail every `every`-th eligible event, at most `budget`
 * times (0 = unlimited), for one thread (the usual case: a self-test
 * targets itself) or for all. Eligible events: kmalloc/kmem_cache_alloc
 * return NULL; blk_submit returns -EIO before the driver; bio_complete
 * turns a success into -EIO. Configured by the kernel API or the boot
 * parameter opt/cosmo/faultinject ("kind:every[:budget],..."), reported by
 * sysctl debug.faultinject. Compiled out of release builds.
 */

#ifndef KERNEL_FAULTINJECT_H
#define KERNEL_FAULTINJECT_H

#include <kernel/compiler.h>

#ifndef CONFIG_FAULTINJECT
#define CONFIG_FAULTINJECT CONFIG_DEBUG
#endif

enum fi_kind {
    FI_KMALLOC,
    FI_BLK_SUBMIT,
    FI_BLK_COMPLETE,
    FI_KIND_COUNT,
};

struct thread;

struct fi_stats {
    unsigned every;
    unsigned budget;      /* remaining; 0 with `every` set means unlimited */
    bool only_thread;
    uint64_t seen;        /* eligible events while the rule was armed */
    uint64_t hits;        /* failures injected */
};

#if CONFIG_FAULTINJECT

/* Arm a rule. `every` 0 disarms. `only` NULL: every thread. */
void faultinject_set(enum fi_kind kind, unsigned every, unsigned budget, struct thread *only);
void faultinject_clear(enum fi_kind kind);
void faultinject_clear_all(void);
void faultinject_stats(enum fi_kind kind, struct fi_stats *out);
const char *faultinject_kind_name(enum fi_kind kind);

/* The hook the injection points call: true when this event must fail. */
bool faultinject_should_fail(enum fi_kind kind);

/* Parse "kind:every[:budget],..." (the boot parameter). Returns 0 or -EINVAL. */
int faultinject_configure(const char *spec);

/* Apply the boot parameter, if any. Called before the self-tests. */
void faultinject_init(void);

/* Text for sysctl debug.faultinject; returns the length. */
int faultinject_sysctl(char *out, size_t n);

#else

static inline bool faultinject_should_fail(enum fi_kind kind) { (void)kind; return false; }
static inline void faultinject_init(void) {}
static inline int faultinject_sysctl(char *out, size_t n) { (void)out; (void)n; return -1; }

#endif

#endif /* KERNEL_FAULTINJECT_H */
