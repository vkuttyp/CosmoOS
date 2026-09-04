/*
 * selftest.h - Boot-time kernel self-tests.
 *
 * Runs when CONFIG_SELFTEST=1 (debug builds by default). Each test prints
 * one "SELFTEST: <name> ... ok|FAIL" line and the run ends with
 * "SELFTEST: PASS (n tests)" or "SELFTEST: FAIL (k of n)". The QEMU
 * harness keys on these lines. Tests must leave the kernel in the state
 * they found it.
 */

#ifndef KERNEL_SELFTEST_H
#define KERNEL_SELFTEST_H

#include <stdbool.h>

/* Returns the number of failed tests. */
int selftest_run_all(void);

/* Test entry points provided by subsystems. Each returns true on success
 * or sets *reason to an immortal string and returns false. */
bool selftest_pmm(const char **reason);
bool selftest_vmm(const char **reason);
bool selftest_kmalloc(const char **reason);

#endif /* KERNEL_SELFTEST_H */
