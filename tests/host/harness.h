/*
 * harness.h - Minimal host unit-test harness for kernel algorithms.
 *
 * Tests run under ASan and UBSan. A kernel panic() inside code under test
 * longjmps back to the harness, so EXPECT_PANIC() can assert that misuse
 * is detected without killing the process.
 */

#ifndef COSMO_HOST_HARNESS_H
#define COSMO_HOST_HARNESS_H

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct host_test {
    const char *name;
    void (*fn)(void);
};

extern int harness_failures;
extern jmp_buf harness_panic_jmp;
extern bool harness_expect_panic;
extern char harness_last_panic[256];
extern int harness_klog_min;   /* klog lines below this level (enum klog_level) are dropped */

void harness_fail(const char *file, int line, const char *expr);
int harness_run(const struct host_test *tests, size_t count);

/* Drop every spinlock the code under test still holds (shim_spinlock.c).
 * Called by panic() before unwinding into EXPECT_PANIC. */
void harness_release_all_locks(void);

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond))                                                           \
            harness_fail(__FILE__, __LINE__, #cond);                           \
    } while (0)

/* Run `stmt`; pass if it panics, fail if it returns. */
#define EXPECT_PANIC(stmt)                                                     \
    do {                                                                       \
        harness_expect_panic = true;                                           \
        if (setjmp(harness_panic_jmp) == 0) {                                  \
            stmt;                                                              \
            harness_expect_panic = false;                                      \
            harness_fail(__FILE__, __LINE__, "expected panic: " #stmt);        \
        }                                                                      \
        harness_expect_panic = false;                                          \
    } while (0)

/* Host-side page-frame arena shared by tests that need real memory
 * behind struct page. */
void host_arena_init(size_t bytes);
void host_arena_destroy(void);
uint64_t host_arena_free_pages(void);

#endif /* COSMO_HOST_HARNESS_H */
