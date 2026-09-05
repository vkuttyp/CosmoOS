/*
 * fuzz.h - The contract between a fuzz target and its driver
 * (docs/verification/design.md, "Fuzzing").
 *
 * A target defines LLVMFuzzerTestOneInput (libFuzzer's entry point) and
 * fuzz_seed, which writes the i-th programmatic seed (0 past the last).
 * The portable driver (driver.c) or libFuzzer (-fsanitize=fuzzer) supplies
 * main. Targets never return an error: they survive or the sanitizer
 * aborts.
 */

#ifndef COSMO_FUZZ_H
#define COSMO_FUZZ_H

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap);

/* Optional: per-process setup before the first input (libFuzzer calls it
 * LLVMFuzzerInitialize). Weak; the driver calls it when defined. */
int LLVMFuzzerInitialize(int *argc, char ***argv);

/* Optional: the largest input the target wants (the driver's default is
 * 64 KiB). Weak. */
size_t fuzz_max_len(void);

/* A target's own check: prints and aborts (the sanitizer's exit code is
 * what CI sees; abort() gives a stack too). */
#define FUZZ_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond))                                                           \
            fuzz_fail(__FILE__, __LINE__, #cond);                              \
    } while (0)
void fuzz_fail(const char *file, int line, const char *expr);

#endif /* COSMO_FUZZ_H */
